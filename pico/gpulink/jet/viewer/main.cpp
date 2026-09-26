#include "Runtime.hpp"
#include "Viewer.hpp"

extern "C" void app_main() {
    Esp32Jet::start(Viewer::init, Viewer::update);
}
