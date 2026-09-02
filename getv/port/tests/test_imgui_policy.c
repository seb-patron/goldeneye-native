/* Developer-overlay and console availability policy, without SDL, ImGui, a window, or a ROM. */
#include <stdio.h>

#include "ge_imgui_policy.c"

static int failures;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    GeImguiPolicy policy;
    printf("test_imgui_policy\n");

    policy = geImguiPolicyResolve(1, NULL);
    check("compiled console is available with developer setting absent",
          policy.console_ui_enabled, 1);
    check("absent developer setting keeps overlay off", policy.developer_overlay_enabled, 0);

    policy = geImguiPolicyResolve(1, "0");
    check("compiled console is available with developer setting off",
          policy.console_ui_enabled, 1);
    check("zero developer setting keeps overlay off", policy.developer_overlay_enabled, 0);

    policy = geImguiPolicyResolve(1, "1");
    check("compiled console remains available with developer setting on",
          policy.console_ui_enabled, 1);
    check("developer setting enables overlay", policy.developer_overlay_enabled, 1);

    policy = geImguiPolicyResolve(0, "1");
    check("no-ImGui build exposes no console UI", policy.console_ui_enabled, 0);
    check("no-ImGui build exposes no developer overlay", policy.developer_overlay_enabled, 0);

    return failures ? 1 : 0;
}
