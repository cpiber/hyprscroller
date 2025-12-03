#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include "overview.h"

extern HANDLE PHANDLE;

inline CFunctionHook* g_pVisibleOnMonitorHook = nullptr;
inline CFunctionHook* g_pRenderLayerHook = nullptr;
inline CFunctionHook* g_pLogicalBoxHook = nullptr;
inline CFunctionHook* g_pRenderSoftwareCursorsForHook = nullptr;
inline CFunctionHook* g_pGetMonitorFromVectorHook = nullptr;
inline CFunctionHook* g_pClosestValidHook = nullptr;
inline CFunctionHook* g_pRenderMonitorHook = nullptr;
inline CFunctionHook* g_pGetCursorPosForMonitorHook = nullptr;

Overview *overviews = nullptr;

typedef bool (*origVisibleOnMonitor)(void *thisptr, PHLMONITOR monitor);
typedef void (*origRenderLayer)(void *thisptr, PHLLS pLayer, PHLMONITOR pMonitor, const Time::steady_tp&, bool popups, bool lockscreen);
typedef CBox (*origLogicalBox)(CMonitor *thisptr);
typedef void (*origRenderSoftwareCursorsFor)(void *thisptr, PHLMONITOR pMonitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool forceRender);
typedef Vector2D (*origClosestValid)(void *thisptr, const Vector2D &pos);
typedef PHLMONITOR (*origGetMonitorFromVector)(void *thisptr, const Vector2D& point);
typedef void (*origRenderMonitor)(CHyprRenderer *thisptr, PHLMONITOR pMonitor, bool commit);
typedef Vector2D (*origGetCursorPosForMonitor)(void *thisptr, PHLMONITOR pMonitor);

class OverviewPassElement : public IPassElement {
public:
    struct OverviewModifData {
        std::optional<SRenderModifData> renderModif;
    };

    OverviewPassElement(const OverviewModifData &data) : data(data) {}
    virtual ~OverviewPassElement() = default;

    virtual void draw(const CRegion& damage) {
        if (data.renderModif.has_value())
            g_pHyprOpenGL->m_renderData.renderModif = *data.renderModif;
    }
    virtual bool needsLiveBlur() { return false; };
    virtual bool needsPrecomputeBlur() { return false; };
    virtual bool undiscardable() { return true; };

    virtual const char* passName() {
        return "OverviewPassElement";
    }

private:
    OverviewModifData data;
};

// Needed to show windows that are outside of the viewport
static bool hookVisibleOnMonitor(void *thisptr, PHLMONITOR monitor) {
    CWindow *window = static_cast<CWindow *>(thisptr);
    if (overviews->overview_enabled(window->workspaceID())) {
        return true;
    }
    return ((origVisibleOnMonitor)(g_pVisibleOnMonitorHook->m_original))(thisptr, monitor);
}

// Needed to undo the monitor scale to render layers at the original scale
static void hookRenderLayer(void *thisptr, PHLLS layer, PHLMONITOR monitor, const Time::steady_tp& time, bool popups, bool lockscreen) {
    static auto* const *ENABLE_RENDER = (Hyprlang::INT* const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:overview_render_layers")->getDataStaticPtr();
    if (!**ENABLE_RENDER)
        return;
    WORKSPACEID workspace = monitor->activeSpecialWorkspaceID();
    if (!workspace)
        workspace = monitor->activeWorkspaceID();
    auto &data = overviews->data_for(workspace);
    if (data.overview) {
        Vector2D monitor_size = monitor->m_size;
        monitor->m_size = monitor->m_size * data.scale_i;
        SRenderModifData modif_data;
        modif_data.modifs.push_back({SRenderModifData::eRenderModifType::RMOD_TYPE_SCALE, data.scale_i});
        modif_data.enabled = true;
        g_pHyprRenderer->m_renderPass.add(makeUnique<OverviewPassElement>(OverviewPassElement::OverviewModifData(modif_data)));
        g_pHyprRenderer->damageMonitor(monitor);
        ((origRenderLayer)(g_pRenderLayerHook->m_original))(thisptr, layer, monitor, time, popups, lockscreen);
        g_pHyprRenderer->m_renderPass.add(makeUnique<OverviewPassElement>(OverviewPassElement::OverviewModifData(SRenderModifData())));
        monitor->m_size = monitor_size;
        return;
    }
    ((origRenderLayer)(g_pRenderLayerHook->m_original))(thisptr, layer, monitor, time, popups, lockscreen);
}

// Needed to scale the range of the cursor in overview mode to cover the whole area.
static CBox hookLogicalBox(CMonitor *thisptr) {
    WORKSPACEID workspace = thisptr->activeSpecialWorkspaceID();
    if (!workspace)
        workspace = thisptr->activeWorkspaceID();
    auto &data = overviews->data_for(workspace);
    if (data.overview)
        return {thisptr->m_position, thisptr->m_size * data.scale_i};
    return ((origLogicalBox)(g_pLogicalBoxHook->m_original))(thisptr);
}

// Needed to render the software cursor only on the correct monitors.
static void hookRenderSoftwareCursorsFor(void *thisptr, PHLMONITOR monitor, const Time::steady_tp& now, CRegion& damage, std::optional<Vector2D> overridePos, bool forceRender) {
    // Should render the cursor for all the extent of the workspace, and only on
    // overview workspaces when there is one active, and it is in the current monitor.
    PHLMONITOR last = Desktop::focusState()->monitor();

    if (monitor == last) {
        // Render cursor
        WORKSPACEID workspace = monitor->activeSpecialWorkspaceID();
        if (!workspace)
            workspace = monitor->activeWorkspaceID();
        auto &data = overviews->data_for(workspace);
        Vector2D monitor_size = monitor->m_size;
        monitor->m_size = monitor->m_size * data.scale_i;
        ((origRenderSoftwareCursorsFor)(g_pRenderSoftwareCursorsForHook->m_original))(thisptr, monitor, now, damage, overridePos, forceRender);
        monitor->m_size = monitor_size;
    }
}

// Needed to fake an overview monitor's desktop contains all its windows
// instead of some of them being in the other monitor.
static Vector2D hookClosestValid(void *thisptr, const Vector2D& pos) {
    PHLMONITOR last = Desktop::focusState()->monitor();
    WORKSPACEID workspace = last->activeSpecialWorkspaceID();
    if (!workspace)
        workspace = last->activeWorkspaceID();
    bool overview_enabled = overviews->overview_enabled(workspace);
    if (overview_enabled) {
        CBox bounds = last->logicalBox();
        Vector2D ret = pos;
        if (ret.x < bounds.x) ret.x = bounds.x;
        if (ret.x > bounds.x + bounds.w) ret.x = bounds.x + bounds.w;
        if (ret.y < bounds.y) ret.y = bounds.y;
        if (ret.y > bounds.y + bounds.h) ret.y = bounds.y + bounds.h;
        return ret;
    }
    return ((origClosestValid)(g_pClosestValidHook->m_original))(thisptr, pos);
}

// Needed to select the correct monitor for a cursor when two can contain it.
static PHLMONITOR hookGetMonitorFromVector(void *thisptr, const Vector2D& point) {
    CCompositor *compositor = static_cast<CCompositor *>(thisptr);
    // First, see if the current monitor contains the point
    PHLMONITOR last = Desktop::focusState()->monitor();
    PHLMONITOR mon;
    for (auto const& m : compositor->m_monitors) {
        WORKSPACEID workspace = m->activeSpecialWorkspaceID();
        if (!workspace)
            workspace = m->activeWorkspaceID();
        auto &data = overviews->data_for(workspace);
        Vector2D m_size = data.overview ? m->m_size * data.scale_i : m->m_size;
        // If the monitor contains the point
        if (CBox{m->m_position, m_size}.containsPoint(point)) {
            // Priority for last monitor
            if (m == last) {
                return last;
            }
            // Priority for monitor running overview
            if (data.overview) {
                mon = m;
            } else if (!mon) {
                mon = m;
            }
        }
    }
    if (mon)
        return mon;

    float      bestDistance = 0.f;
    PHLMONITOR pBestMon;

    for (auto const& m : compositor->m_monitors) {
        float dist = vecToRectDistanceSquared(point, m->m_position, m->m_position + m->m_size);

        if (dist < bestDistance || !pBestMon) {
            bestDistance = dist;
            pBestMon     = m;
        }
    }

    if (!pBestMon) { // ?????
        //Debug::log(WARN, "getMonitorFromVector no close mon???");
        return compositor->m_monitors.front();
    }

    return pBestMon;
}

static void hookRenderMonitor(CHyprRenderer *thisptr, PHLMONITOR monitor, bool commit) {
    WORKSPACEID workspace = monitor->activeSpecialWorkspaceID();
    if (!workspace)
        workspace = monitor->activeWorkspaceID();
    auto &data = overviews->data_for(workspace);
    float scale = monitor->m_scale;
    if (data.overview)
        monitor->m_scale *= data.scale;
    ((origRenderMonitor)(g_pRenderMonitorHook->m_original))(thisptr, monitor, commit);
    monitor->m_scale = scale;
}

// Needed to render the HW cursor at the right position
static Vector2D hookGetCursorPosForMonitor(void *thisptr, PHLMONITOR monitor) {
    if (Desktop::focusState()->monitor() != monitor)
        return { 0.0, monitor->m_size.y };

    WORKSPACEID workspace = monitor->activeSpecialWorkspaceID();
    if (!workspace)
        workspace = monitor->activeWorkspaceID();
    auto &data = overviews->data_for(workspace);
    auto monitor_scale = monitor->m_scale;
    if (data.overview)
        monitor->m_scale *= data.scale;
    Vector2D pos = ((origGetCursorPosForMonitor)(g_pGetCursorPosForMonitorHook->m_original))(thisptr, monitor);
    monitor->m_scale = monitor_scale;
    return pos;
}



#define DO_HOOK(name_capital, name) do { \
    auto FNS = HyprlandAPI::findFunctionsByName(PHANDLE, #name); \
    if (!FNS.empty()) { \
        g_p ## name_capital ## Hook = HyprlandAPI::createFunctionHook(PHANDLE, FNS[0].address, (bool *)hook ## name_capital); \
        if (g_p ## name_capital ## Hook == nullptr) { \
            Debug::log(WARN, "[hyprscroller] Overview: Hook of " #name " failed, function found but hook not successfull"); \
            return; \
        } \
    } else { \
        Debug::log(WARN, "[hyprscroller] Overview: Hook of " #name " failed, function not found"); \
        return; \
    } \
} while (0)


Overview::Overview() : initialized(false)
{
    // Hook bool CWindow::visibleOnMonitor(PHLMONITOR pMonitor)
    DO_HOOK(VisibleOnMonitor, visibleOnMonitor);
    DO_HOOK(RenderLayer, renderLayer);
    DO_HOOK(LogicalBox, logicalBox);
    DO_HOOK(RenderSoftwareCursorsFor, renderSoftwareCursorsFor);
    DO_HOOK(GetMonitorFromVector, getMonitorFromVector);
    DO_HOOK(ClosestValid, closestValid);
    DO_HOOK(RenderMonitor, renderMonitor);
    DO_HOOK(GetCursorPosForMonitor, getCursorPosForMonitor);

    initialized = true;
}

Overview::~Overview()
{
    if (overview_enabled()) {
        disable_hooks();
    }

    if (g_pClosestValidHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pClosestValidHook);
        g_pClosestValidHook = nullptr;
    }

    if (g_pGetMonitorFromVectorHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pGetMonitorFromVectorHook);
        g_pGetMonitorFromVectorHook = nullptr;
    }

    if (g_pRenderSoftwareCursorsForHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderSoftwareCursorsForHook);
        g_pRenderSoftwareCursorsForHook = nullptr;
    }

    if (g_pLogicalBoxHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pLogicalBoxHook);
        g_pLogicalBoxHook = nullptr;
    }

    if (g_pRenderLayerHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderLayerHook);
        g_pRenderLayerHook = nullptr;
    }

    if (g_pVisibleOnMonitorHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pVisibleOnMonitorHook);
        g_pVisibleOnMonitorHook = nullptr;
    }

    if (g_pRenderMonitorHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderMonitorHook);
        g_pRenderMonitorHook = nullptr;
    }

    if (g_pGetCursorPosForMonitorHook != nullptr) {
        /* bool success = */HyprlandAPI::removeFunctionHook(PHANDLE, g_pGetCursorPosForMonitorHook);
        g_pGetCursorPosForMonitorHook = nullptr;
    }

    initialized = false;
}

bool Overview::enable(WORKSPACEID workspace)
{
    if (!initialized)
        return false;
    if (!overview_enabled()) {
        if (!enable_hooks())
            return false;
    }
    auto &data = data_for(workspace);
    data.overview = true;
    return true;
}

void Overview::disable(WORKSPACEID workspace)
{
    if (!initialized)
        return;
    for (auto &w : _workspaceData) {
        if (w.workspace == workspace) {
            w.overview = false;
            w.scale = 1.0f;
            w.scale_i = 1.0f;
        }
    }
    if (!overview_enabled()) {
        disable_hooks();
    }
}

bool Overview::overview_enabled(WORKSPACEID workspace) const
{
    if (!initialized)
        return false;
    for (auto &w : _workspaceData) {
        if (w.workspace == workspace)
            return w.overview;
    }
    return false;
}

void Overview::set_scale(WORKSPACEID workspace, float scale)
{
    auto &data = data_for(workspace);
    data.scale = scale;
    data.scale_i = 1.0f / scale;
}

Overview::OverviewData& Overview::data_for(WORKSPACEID workspace)
{
    for (auto &w : _workspaceData) {
        if (w.workspace == workspace)
            return w;
    }
    _workspaceData.push_back({.workspace=workspace,.scale=1.0f,.scale_i=1.0f});
    return _workspaceData.back();
}

bool Overview::overview_enabled() const
{
    for (auto &workspace : _workspaceData) {
        if (workspace.overview)
            return true;
    }
    return false;
}

bool Overview::enable_hooks()
{
    if (initialized &&
        g_pVisibleOnMonitorHook != nullptr && g_pVisibleOnMonitorHook->hook() &&
        g_pRenderLayerHook != nullptr && g_pRenderLayerHook->hook() &&
        g_pLogicalBoxHook != nullptr && g_pLogicalBoxHook->hook() &&
        g_pRenderSoftwareCursorsForHook != nullptr && g_pRenderSoftwareCursorsForHook->hook() &&
        g_pClosestValidHook != nullptr && g_pClosestValidHook->hook() &&
        g_pGetMonitorFromVectorHook != nullptr && g_pGetMonitorFromVectorHook->hook() &&
        g_pRenderMonitorHook != nullptr && g_pRenderMonitorHook->hook() &&
        g_pGetCursorPosForMonitorHook != nullptr && g_pGetCursorPosForMonitorHook->hook()) {
        return true;
    }
    return false;
}

void Overview::disable_hooks()
{
    if (!initialized)
        return;

    if (g_pVisibleOnMonitorHook != nullptr) g_pVisibleOnMonitorHook->unhook();
    if (g_pRenderLayerHook != nullptr) g_pRenderLayerHook->unhook();
    if (g_pLogicalBoxHook != nullptr) g_pLogicalBoxHook->unhook();
    if (g_pRenderSoftwareCursorsForHook != nullptr) g_pRenderSoftwareCursorsForHook->unhook();
    if (g_pClosestValidHook != nullptr) g_pClosestValidHook->unhook();
    if (g_pGetMonitorFromVectorHook != nullptr) g_pGetMonitorFromVectorHook->unhook();
    if (g_pRenderMonitorHook != nullptr) g_pRenderMonitorHook->unhook();
    if (g_pGetCursorPosForMonitorHook != nullptr) g_pGetCursorPosForMonitorHook->unhook();
}

