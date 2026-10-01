/*
 * portal.c - see portal.h. A request is read whole (its head, and a POST's
 * body by its Content-Length), answered in one write and the connection
 * closed.
 */
#include "portal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#define CONNECTIONS	4
#define REQUEST_MAX	1536

typedef struct
{
	struct tcp_pcb *pcb;			/* NULL: free */
	unsigned len, left;			/* of the request; of the answer, not acknowledged yet */
	bool answered;
	char request[REQUEST_MAX + 1];
} connection_t;

static struct tcp_pcb *listener;
static connection_t connections[CONNECTIONS];
static char url[32];
static const char (*net_names)[PORTAL_SSID_MAX + 1];
static unsigned net_count;
static const char *net_note;
static char form_ssid[PORTAL_SSID_MAX + 1], form_password[PORTAL_PASSWORD_MAX + 1];
static bool form_came;
static unsigned served;

static const char page_head[] =
	"<!doctype html><html><head><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'><title>Wi-Fi setup</title><style>"
	"body{font:17px system-ui,sans-serif;margin:0;padding:24px;background:#14161a;color:#e8eaed}"
	"h1{font-size:22px;margin:0 0 4px}p{color:#9aa0a6;margin:0 0 20px}"
	"label{display:block;margin:0 0 16px}"
	"input,select{display:block;width:100%;box-sizing:border-box;margin-top:6px;padding:12px;font-size:17px;"
	"border:1px solid #3c4043;border-radius:8px;background:#202124;color:#e8eaed}"
	"button{width:100%;padding:14px;font-size:17px;border:0;border-radius:8px;background:#1a73e8;color:#fff}"
	".e{color:#f28b82;margin:0 0 16px}</style></head><body>";

/* text into HTML */
static unsigned escape (char *out, unsigned max, const char *text)
{
	unsigned n = 0;
	for (; *text && n + 7 < max; text++)
	{
		const char *e = *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;"
			      : *text == '"' ? "&quot;" : *text == '\'' ? "&#39;" : NULL;
		if (e)
		{
			n += (unsigned) snprintf (out + n, max - n, "%s", e);
		}
		else
		{
			out[n++] = *text;
		}
	}
	out[n] = '\0';
	return n;
}

/* a form's field: + and %XX undone, at most max bytes */
static void field (const char *body, const char *name, char *out, unsigned max)
{
	unsigned n = 0, len = (unsigned) strlen (name);
	out[0] = '\0';
	for (const char *p = body; p; p = strchr (p, '&'), p = p ? p + 1 : NULL)
	{
		if (strncmp (p, name, len) != 0 || p[len] != '=')
		{
			continue;
		}
		for (p += len + 1; *p && *p != '&' && n < max; p++)
		{
			if (*p == '%' && isxdigit ((unsigned char) p[1]) && isxdigit ((unsigned char) p[2]))
			{
				const char hex[3] = {p[1], p[2], '\0'};
				out[n++] = (char) strtoul (hex, NULL, 16);
				p += 2;
			}
			else
			{
				out[n++] = *p == '+' ? ' ' : *p;
			}
		}
		break;
	}
	out[n] = '\0';
}

static void release (connection_t *c)
{
	if (c->pcb)
	{
		tcp_arg (c->pcb, NULL);
		tcp_recv (c->pcb, NULL);
		tcp_sent (c->pcb, NULL);
		tcp_err (c->pcb, NULL);
		tcp_poll (c->pcb, NULL, 0);
		if (tcp_close (c->pcb) != ERR_OK)
		{
			tcp_abort (c->pcb);
		}
		c->pcb = NULL;
	}
}

static void send (connection_t *c, const char *status, const char *extra, const char *body, unsigned body_len)
{
	static char head[256];
	unsigned n = (unsigned) snprintf (head, sizeof head,
					  "HTTP/1.1 %s\r\n%sContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
					  "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, extra, body_len);
	c->answered = true;
	c->left = n + body_len;
	if (   tcp_write (c->pcb, head, (u16_t) n, TCP_WRITE_FLAG_COPY | (body_len ? TCP_WRITE_FLAG_MORE : 0)) != ERR_OK
	    || (body_len && tcp_write (c->pcb, body, (u16_t) body_len, TCP_WRITE_FLAG_COPY) != ERR_OK))
	{
		release (c);
		return;
	}
	tcp_output (c->pcb);
	served++;
}

/* the request is whole: the form, what the form sent, or the way to the form */
static void answer (connection_t *c)
{
	static char body[6144];
	char *req = c->request;
	bool get = strncmp (req, "GET ", 4) == 0, post = strncmp (req, "POST ", 5) == 0;
	const char *path = req + (post ? 5 : 4);
	unsigned path_len = (unsigned) strcspn (path, " ?\r\n");

	if (post && path_len == 5 && strncmp (path, "/save", 5) == 0)
	{
		const char *content = strstr (req, "\r\n\r\n");
		content = content ? content + 4 : "";
		field (content, "other", form_ssid, PORTAL_SSID_MAX);		/* the name typed, else the one chosen */
		if (!form_ssid[0])
		{
			field (content, "ssid", form_ssid, PORTAL_SSID_MAX);
		}
		field (content, "pass", form_password, PORTAL_PASSWORD_MAX);
		static char name[PORTAL_SSID_MAX * 6 + 1];
		escape (name, sizeof name, form_ssid);
		unsigned n;
		if (form_ssid[0])
		{
			form_came = true;
			n = (unsigned) snprintf (body, sizeof body,
						 "%s<h1>Connecting</h1><p>The Pico is joining <b>%s</b> now and leaves this "
						 "network. Its screen says how it went; if it didn't work, this network "
						 "comes back and you can try again.</p></body></html>", page_head, name);
		}
		else
		{
			n = (unsigned) snprintf (body, sizeof body,
						 "%s<h1>No network name</h1><p><a href='/'>Back</a></p></body></html>", page_head);
		}
		send (c, "200 OK", "", body, n);
	}
	else if (get && path_len == 1)
	{
		unsigned n = (unsigned) snprintf (body, sizeof body,
						  "%s<h1>Wi-Fi setup</h1><p>Which network should the Pico join?</p>", page_head);
		if (net_note && net_note[0])
		{
			static char note[160];
			escape (note, sizeof note, net_note);
			n += (unsigned) snprintf (body + n, sizeof body - n, "<div class=e>%s</div>", note);
		}
		/* the networks found, the strongest first, in a menu; one that isn't there, by its name */
		n += (unsigned) snprintf (body + n, sizeof body - n,
					  "<form method=post action=/save><label>Network<select name=ssid>"
					  "<option value=''>%s</option>", net_count ? "Choose one" : "None found: type its name below");
		for (unsigned i = 0; i < net_count && n + 600 < sizeof body; i++)
		{
			static char name[PORTAL_SSID_MAX * 6 + 1];
			escape (name, sizeof name, net_names[i]);
			n += (unsigned) snprintf (body + n, sizeof body - n, "<option value=\"%s\">%s</option>", name, name);
		}
		n += (unsigned) snprintf (body + n, sizeof body - n,
					  "</select></label><label>or its name, if it isn't in the list<input name=other "
					  "maxlength=32 autocapitalize=none autocorrect=off spellcheck=false></label>"
					  "<label>Password<input name=pass type=password maxlength=63 "
					  "autocapitalize=none autocorrect=off></label><button>Connect</button></form></body></html>");
		send (c, "200 OK", "", body, n);
	}
	else
	{
		static char location[64];
		snprintf (location, sizeof location, "Location: %s\r\n", url);
		send (c, "302 Found", location, "", 0);
	}
}

/* is the request all here? (a POST: its body too) */
static bool whole (const connection_t *c)
{
	const char *end = strstr (c->request, "\r\n\r\n");
	if (!end)
	{
		return c->len >= REQUEST_MAX;
	}
	if (strncmp (c->request, "POST ", 5) != 0)
	{
		return true;
	}
	unsigned length = 0;
	for (const char *p = c->request; p < end; p++)
	{
		if ((p == c->request || p[-1] == '\n') && strncasecmp (p, "Content-Length:", 15) == 0)
		{
			length = (unsigned) strtoul (p + 15, NULL, 10);
		}
	}
	return c->len >= (unsigned) (end + 4 - c->request) + length || c->len >= REQUEST_MAX;
}

static err_t on_recv (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	connection_t *c = arg;
	if (!c)
	{
		if (p)
		{
			pbuf_free (p);
		}
		return ERR_OK;
	}
	if (!p)							/* closed by the phone */
	{
		release (c);
		return ERR_OK;
	}
	unsigned take = p->tot_len < REQUEST_MAX - c->len ? p->tot_len : REQUEST_MAX - c->len;
	pbuf_copy_partial (p, c->request + c->len, (u16_t) take, 0);
	c->len += take;
	c->request[c->len] = '\0';
	tcp_recved (pcb, p->tot_len);
	pbuf_free (p);
	if (!c->answered && whole (c))
	{
		answer (c);
	}
	return ERR_OK;
}

static err_t on_sent (void *arg, struct tcp_pcb *pcb, u16_t len)
{
	connection_t *c = arg;
	if (c && c->answered)
	{
		c->left = len < c->left ? c->left - len : 0;
		if (!c->left)
		{
			release (c);
		}
	}
	return ERR_OK;
}

static void on_error (void *arg, err_t err)			/* (the pcb is gone already) */
{
	connection_t *c = arg;
	if (c)
	{
		c->pcb = NULL;
	}
}

static err_t on_poll (void *arg, struct tcp_pcb *pcb)		/* idle for 10 s: let go */
{
	connection_t *c = arg;
	if (c)
	{
		release (c);
	}
	return ERR_OK;
}

static err_t on_accept (void *arg, struct tcp_pcb *pcb, err_t err)
{
	if (err != ERR_OK || !pcb)
	{
		return ERR_VAL;
	}
	for (unsigned i = 0; i < CONNECTIONS; i++)
	{
		connection_t *c = &connections[i];
		if (!c->pcb)
		{
			c->pcb = pcb;
			c->len = c->left = 0;
			c->answered = false;
			c->request[0] = '\0';
			tcp_arg (pcb, c);
			tcp_recv (pcb, on_recv);
			tcp_sent (pcb, on_sent);
			tcp_err (pcb, on_error);
			tcp_poll (pcb, on_poll, 20);
			return ERR_OK;
		}
	}
	tcp_abort (pcb);
	return ERR_ABRT;
}

void portal_start (const ip4_addr_t *address, const char (*networks)[PORTAL_SSID_MAX + 1], unsigned count,
		   const char *note)
{
	portal_stop ();
	snprintf (url, sizeof url, "http://%s/", ip4addr_ntoa (address));
	net_names = networks;
	net_count = count < PORTAL_NETWORKS ? count : PORTAL_NETWORKS;
	net_note = note;
	form_came = false;
	struct tcp_pcb *pcb = tcp_new_ip_type (IPADDR_TYPE_V4);
	if (pcb && tcp_bind (pcb, IP_ANY_TYPE, 80) == ERR_OK)
	{
		listener = tcp_listen_with_backlog (pcb, CONNECTIONS);
		if (listener)
		{
			tcp_accept (listener, on_accept);
		}
	}
}

void portal_stop (void)
{
	for (unsigned i = 0; i < CONNECTIONS; i++)
	{
		release (&connections[i]);
	}
	if (listener)
	{
		tcp_close (listener);
		listener = NULL;
	}
}

bool portal_take (char ssid[PORTAL_SSID_MAX + 1], char password[PORTAL_PASSWORD_MAX + 1])
{
	if (!form_came)
	{
		return false;
	}
	form_came = false;
	memcpy (ssid, form_ssid, sizeof form_ssid);
	memcpy (password, form_password, sizeof form_password);
	memset (form_password, 0, sizeof form_password);
	return true;
}

unsigned portal_requests (void)	{ return served; }
