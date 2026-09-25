// plume: an afterburner flame as a volume: a tapered cone along the exhaust
// axis (the mesh's -x), capped with a disc at the nozzle; drawn twice, as the
// wide outer flame and the narrow core inside it
attribute vec4 a_geom;		// x: 0 at the nozzle .. 1 at the tip; y, z: cos, sin around;
				// w: 0 on the cone, 1 + radial fraction on the cap

uniform mat4 u_vp;
uniform mat4 u_model;		// the aircraft's (mesh units to world)
uniform vec3 u_nozzle;		// the nozzle's mouth, mesh units (the nose is +x)
uniform float u_length;		// mesh units
uniform vec2 u_radius;		// mesh units, at the nozzle: half height (y), half width (z)
uniform vec3 u_eye;
uniform float u_time;

varying float v_u;
varying float v_facing;		// |cos| between the surface and the view: the flame's thickness
varying float v_cap;		// radial fraction on the cap, -1 on the cone

void main ()
{
	float u = a_geom.x, cap = a_geom.w - 1.0;
	// bulges a little behind the nozzle, then burns down to a point; ripples
	float r = (1.0 + 0.35 * sin (3.14159 * u)) * (1.0 - pow (u, 1.5))
		* (1.0 + 0.06 * sin (u * 17.0 - u_time * 37.0));
	float rr = cap >= 0.0 ? cap : 1.0;
	vec3 pm = u_nozzle + vec3 (-u * u_length, r * rr * u_radius.x * a_geom.y, r * rr * u_radius.y * a_geom.z);
	vec3 nm = cap >= 0.0 ? vec3 (-1.0, 0.0, 0.0) : vec3 (0.0, a_geom.y / u_radius.x, a_geom.z / u_radius.y);

	vec3 p = (u_model * vec4 (pm, 1.0)).xyz;
	vec3 n = normalize ((u_model * vec4 (nm, 0.0)).xyz);
	v_facing = abs (dot (n, normalize (u_eye - p)));
	v_u = u;
	v_cap = cap >= 0.0 ? cap : -1.0;
	gl_Position = u_vp * vec4 (p, 1.0);
}
