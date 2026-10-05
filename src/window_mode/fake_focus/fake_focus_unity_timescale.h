#pragma once

// =============================================================================
// fake_focus_unity_timescale.h — Unity (IL2CPP) 游戏的「变速」
// =============================================================================
// 为什么除了系统时钟缩放之外还需要它：
//   实测《植物大战僵尸融合版》（Unity IL2CPP）：内联钩 9 个全装上、QPC 被调用 10.6 万次/秒、
//   时钟确实被改成 2 倍 —— 游戏逻辑纹丝不动。
//   原因：Unity 里决定「游戏速度」的是 `Time.timeScale`
//   （`Time.deltaTime = Time.unscaledDeltaTime * Time.timeScale`）。
//   2D 游戏常把 deltaTime 固定成 1/60，那它**根本不从系统时钟来**。
//
// ⚠ 三个必须记住的坑（都真踩过，都属于「同类漏洞」）：
//   1. **`il2cpp_runtime_invoke` 对值类型返回值是「装箱」返回的** —— 直接当 `float*` 读会读到
//      对象头垃圾。必须 `il2cpp_object_unbox(ret)` 再读。
//      ⇒ 推论：**任何「读一下再决定」的保护逻辑，都要先保证读是可靠的**，
//        并且**读失败时绝不能变成「不干活」**（第一版就栽在这：get 读错 → 误判游戏暂停 → 值没设上）。
//   2. **调用线程必须自己 `il2cpp_thread_attach`**：Init 跑在注入线程上，那线程装完就退出；
//      真正调用的是轮询线程。
//   3. **解析要可重试**：注入那一刻 IL2CPP 域/程序集可能还没就绪，一次失败不该永久放弃。
//      所以 Init() 由轮询线程驱动、失败下次再来。
//
// 另外强制 `Application.runInBackground = true`：Unity 独立版默认 **false**，
//   窗口一失焦就**停止更新游戏循环** —— 对「后台窗口模式」是致命的。设不上也不影响变速，故每次重试。
//
// 与游戏自身的暂停共存：游戏把 `Time.timeScale` 设成 0（暂停菜单/失焦暂停）时**不插手**；
//   游戏改了基准值（慢动作等）就跟随它。卸载时恢复进入前的原值。
// =============================================================================

#include <windows.h>

#include <cstdint>

namespace fakefocus {
namespace unity {

// ── IL2CPP 运行时 API（GameAssembly.dll 导出）────────────────────────────
using Il2CppDomain = void;
using Il2CppAssembly = void;
using Il2CppImage = void;
using Il2CppClass = void;
using Il2CppMethodInfo = void;
using Il2CppObject = void;
using Il2CppException = void;
using Il2CppThread = void;

struct Api {
    Il2CppDomain* (*domain_get)() = nullptr;
    const Il2CppAssembly** (*domain_get_assemblies)(Il2CppDomain*, size_t*) = nullptr;
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*) = nullptr;
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*) = nullptr;
    const Il2CppMethodInfo* (*class_get_method_from_name)(
        Il2CppClass*, const char*, int) = nullptr;
    Il2CppObject* (*runtime_invoke)(
        const Il2CppMethodInfo*, void*, void**, Il2CppException**) = nullptr;
    /// ⚠ 返回的是 **Il2CppThread***（不是 domain）：detach 时必须原样还回去。
    Il2CppThread* (*thread_attach)(Il2CppDomain*) = nullptr;
    /// 可选（老版本可能没有）。取不到就只是不 detach。
    void (*thread_detach)(Il2CppThread*) = nullptr;
    void* (*object_unbox)(Il2CppObject*) = nullptr;   // 值类型返回值必须过这一道
};

inline Api g_api{};
inline bool g_apiReady = false;

inline const Il2CppMethodInfo* g_setTimeScale = nullptr;
inline const Il2CppMethodInfo* g_getTimeScale = nullptr;
inline const Il2CppMethodInfo* g_setRunInBackground = nullptr;

inline bool g_available = false;
inline bool g_applied = false;          // 当前是否由我们设过值
inline float g_baseScale = 1.0f;        // 进入前的原值（卸载时恢复）
inline float g_lastSet = -1.0f;         // 我们最近一次写进去的值（用来识别「游戏自己改过」）

// 诊断状态（写进共享内存，宿主读）
inline bool g_getFailed = false;
inline bool g_setFailed = false;
inline bool g_runInBackgroundFound = false;
inline bool g_runInBackgroundSet = false;
inline float g_lastReadScale = -1.0f;   // <0 = 没读到

/// 宿主侧共享内存里的状态字段（DLL 安装时注入）。
inline uint32_t* g_stateSink = nullptr;
inline void SetStateSink(uint32_t* p) { g_stateSink = p; }
inline void PublishState();

/// 本线程 attach 得到的 IL2CPP 线程对象（`il2cpp_thread_detach` 要原样还回去）。
/// 用 thread_local：注入线程与轮询线程都可能 attach，别互相覆盖。
inline thread_local Il2CppThread* t_attachedThread = nullptr;

inline void AttachCurrentThread() {
    if (g_api.thread_attach && g_api.domain_get) {
        t_attachedThread = g_api.thread_attach(g_api.domain_get());
    }
}

/// 线程退出前必须调（没 attach 过就什么都不做）。
///
/// ⚠⚠ 漏掉这一步 = IL2CPP 线程表里留下**已终止线程**的记录 ⇒ 下一次 GC 扫栈
///   就是一次访问违例 ⇒ 整个游戏进程消失、无日志（`exit=0xC0000005`）。
///   ⚠ 特别注意「**装完就退的临时线程**」：注入用的 `CreateRemoteThread`
///     执行完 `FakeFocus_Install` 就结束 —— 绝不能在那种线程上 attach
///     （所以 `InitRealTimeApi()` 里不再调 `unity::Init()`）。
inline void DetachCurrentThread() {
    if (g_api.thread_detach && t_attachedThread) {
        g_api.thread_detach(t_attachedThread);
        t_attachedThread = nullptr;
    }
}

/// 把 invoke 的返回值取成 float。**必须处理装箱**：值类型返回值是 boxed object。
inline float UnboxFloat(void* ret) {
    if (!ret) return 0.0f;
    if (g_api.object_unbox) {
        void* raw = g_api.object_unbox(ret);
        if (raw) return *reinterpret_cast<float*>(raw);
    }
    return *reinterpret_cast<float*>(ret);
}

/// 返回 -1.0f 表示读失败（**调用方必须把「失败」和「值为 0」区分开**）。
inline float InvokeGet() {
    if (!g_api.runtime_invoke || !g_getTimeScale) return -1.0f;
    void* args[1] = { nullptr };
    Il2CppException* exc = nullptr;
    void* ret = g_api.runtime_invoke(g_getTimeScale, nullptr, args, &exc);
    if (exc || !ret) {
        g_getFailed = true;
        return -1.0f;
    }
    return UnboxFloat(ret);
}

inline bool InvokeSet(float v) {
    if (!g_api.runtime_invoke || !g_setTimeScale) return false;
    void* args[1] = { &v };
    Il2CppException* exc = nullptr;
    g_api.runtime_invoke(g_setTimeScale, nullptr, args, &exc);
    if (exc) {
        g_setFailed = true;
        return false;
    }
    return true;
}

inline bool InvokeSetBool(const Il2CppMethodInfo* m, bool v) {
    if (!g_api.runtime_invoke || !m) return false;
    void* args[1] = { &v };
    Il2CppException* exc = nullptr;
    g_api.runtime_invoke(m, nullptr, args, &exc);
    return exc == nullptr;
}

/// 解析 IL2CPP API（只做一次）。非 Unity 目标返回 false。
inline bool ResolveApi() {
    if (g_apiReady) return true;
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) return false;
#define QST_IL2CPP(field, name) \
    g_api.field = reinterpret_cast<decltype(g_api.field)>(GetProcAddress(ga, name))
    QST_IL2CPP(domain_get, "il2cpp_domain_get");
    QST_IL2CPP(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    QST_IL2CPP(assembly_get_image, "il2cpp_assembly_get_image");
    QST_IL2CPP(class_from_name, "il2cpp_class_from_name");
    QST_IL2CPP(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    QST_IL2CPP(runtime_invoke, "il2cpp_runtime_invoke");
    QST_IL2CPP(thread_attach, "il2cpp_thread_attach");
    QST_IL2CPP(thread_detach, "il2cpp_thread_detach");   // 可选，不参与下面的必填检查
    QST_IL2CPP(object_unbox, "il2cpp_object_unbox");
#undef QST_IL2CPP
    if (!g_api.domain_get || !g_api.domain_get_assemblies || !g_api.assembly_get_image
        || !g_api.class_from_name || !g_api.class_get_method_from_name
        || !g_api.runtime_invoke) {
        return false;
    }
    g_apiReady = true;
    return true;
}

/// 解析 `UnityEngine.Time` / `Application`。**可反复调用**：
/// 注入那一刻 IL2CPP 域或程序集可能还没就绪，一次失败不该永久放弃。
inline bool Init() {
    if (g_available) return true;
    if (!ResolveApi()) return false;
    Il2CppDomain* domain = g_api.domain_get();
    if (!domain) return false;
    AttachCurrentThread();

    size_t count = 0;
    const Il2CppAssembly** asms = g_api.domain_get_assemblies(domain, &count);
    if (!asms || count == 0) return false;

    Il2CppClass* timeCls = nullptr;
    Il2CppClass* appCls = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (!asms[i]) continue;
        const Il2CppImage* image = g_api.assembly_get_image(asms[i]);
        if (!image) continue;
        if (!timeCls) timeCls = g_api.class_from_name(image, "UnityEngine", "Time");
        if (!appCls) appCls = g_api.class_from_name(image, "UnityEngine", "Application");
        if (timeCls && appCls) break;
    }
    if (timeCls) {
        if (!g_setTimeScale) {
            g_setTimeScale = g_api.class_get_method_from_name(timeCls, "set_timeScale", 1);
        }
        if (!g_getTimeScale) {
            g_getTimeScale = g_api.class_get_method_from_name(timeCls, "get_timeScale", 0);
        }
    }
    if (appCls && !g_setRunInBackground) {
        g_setRunInBackground =
            g_api.class_get_method_from_name(appCls, "set_runInBackground", 1);
        g_runInBackgroundFound = (g_setRunInBackground != nullptr);
    }
    if (!g_setTimeScale) return false;   // 下次轮询再试
    g_available = true;

    // 记下进入前的基准值。读不到就用 1.0，**绝不让「读不准」变成「不设值」**。
    const float base = InvokeGet();
    g_lastReadScale = base;
    g_baseScale = base > 0.0f ? base : 1.0f;
    g_lastSet = -1.0f;
    PublishState();
    return true;
}

inline bool Available() { return g_available; }

/// 设置 Unity 时间倍率。speed = 1.0 表示恢复。
/// 由轮询线程调用；`Init()` 失败会每次重试。
inline bool SetTimeScale(float speed) {
    if (speed <= 0.0f) return false;
    if (!Init()) return false;
    // ⚠ 必须在**调用线程**上挂域（Init 跑在注入线程上，那线程装完就退了）。
    AttachCurrentThread();

    // runInBackground：Unity 独立版默认 false，窗口失焦就停更游戏循环。
    // 设不上不影响变速，所以每次重试直到成功。
    if (!g_runInBackgroundSet && g_setRunInBackground) {
        g_runInBackgroundSet = InvokeSetBool(g_setRunInBackground, true);
    }

    // 读当前值，用来跟游戏自己的暂停/慢动作共存。
    // 注意三档语义：<0 = 读失败；==0 = 游戏在暂停；>0 = 正常值。
    const float cur = InvokeGet();
    if (cur >= 0.0f) {
        if (cur == 0.0f) { PublishState(); return false; }  // 游戏暂停 → 尊重它，不插手
        if (cur != g_lastSet) g_baseScale = cur;            // 游戏改过基准（慢动作等）→ 跟随
    }
    // cur < 0（读失败）⇒ 用记住的基准，**绝不因此不设值**（第一版的教训）。
    const float want = g_baseScale * speed;
    const bool ok = InvokeSet(want);
    if (ok) {
        g_applied = true;
        g_lastSet = want;
    }
    PublishState();
    return ok;
}

/// 恢复进入前的原值。作用域退出 / 卸载时无条件调用。
inline void Restore() {
    if (!g_available || !g_applied) return;
    g_applied = false;
    // ⚠ `Restore()` 会被 `StopPoll()` → `PollOnce(0)` 在**调用方线程**上执行
    //   （那是宿主的远程线程，装完就退）⇒ 这里必须「用完就还」：
    //   本来没 attach 过就自己 detach，本来已 attach（常驻轮询线程）就别动它。
    const bool wasAttached = (t_attachedThread != nullptr);
    AttachCurrentThread();
    // 只有「当前值还是我们写的那个」才恢复 —— 游戏自己改过（暂停/菜单）就别动它。
    const float cur = InvokeGet();
    if (cur >= 0.0f && cur != g_lastSet) {
        PublishState();
        if (!wasAttached) DetachCurrentThread();
        return;
    }
    InvokeSet(g_baseScale);
    g_lastSet = g_baseScale;
    PublishState();
    if (!wasAttached) DetachCurrentThread();
}

/// 诊断（宿主读）：
///   bits 0..15  = 进入前读到的 timeScale ×1000（0xFFFF = 读不到）
///   bit16 = Unity 变速可用   bit17 = 已由我们设过
///   bit18 = get 失败过       bit19 = set 失败过
///   bit20 = runInBackground 已设   bit21 = runInBackground 方法找到
inline uint32_t State() {
    uint32_t v = 0;
    const int milli = g_lastReadScale < 0.0f
        ? 0xFFFF
        : static_cast<int>(g_lastReadScale * 1000.0f + 0.5f);
    v |= static_cast<uint32_t>(milli & 0xFFFF);
    if (g_available) v |= 1u << 16;
    if (g_applied) v |= 1u << 17;
    if (g_getFailed) v |= 1u << 18;
    if (g_setFailed) v |= 1u << 19;
    if (g_runInBackgroundSet) v |= 1u << 20;
    if (g_runInBackgroundFound) v |= 1u << 21;
    return v;
}

inline void PublishState() {
    if (g_stateSink) *g_stateSink = State();
}

}  // namespace unity
}  // namespace fakefocus
