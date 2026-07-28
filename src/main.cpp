#include <windows.h>

#include "app/AppHost.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  olouie::app::AppHost host(instance);
  return host.Run(show_command);
}
