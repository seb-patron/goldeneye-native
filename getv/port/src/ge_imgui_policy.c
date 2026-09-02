#include <string.h>

#include "ge_imgui_policy.h"

GeImguiPolicy geImguiPolicyResolve(int imgui_compiled, const char *developer_tools_value)
{
    GeImguiPolicy policy;
    policy.console_ui_enabled = imgui_compiled != 0;
    policy.developer_overlay_enabled =
        policy.console_ui_enabled && developer_tools_value != NULL &&
        developer_tools_value[0] != '\0' && strcmp(developer_tools_value, "0") != 0;
    return policy;
}
