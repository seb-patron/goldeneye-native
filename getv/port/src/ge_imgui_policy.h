/* Testable runtime policy shared by the optional ImGui console and developer overlay. */
#ifndef GE_IMGUI_POLICY_H
#define GE_IMGUI_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GeImguiPolicy {
    int console_ui_enabled;
    int developer_overlay_enabled;
} GeImguiPolicy;

/* developer_tools_value is the resolved GETV_IMGUI value. ImGui availability controls the
 * shared runtime; the developer setting controls only the observational overlay. */
GeImguiPolicy geImguiPolicyResolve(int imgui_compiled, const char *developer_tools_value);

#ifdef __cplusplus
}
#endif

#endif /* GE_IMGUI_POLICY_H */
