#include "helper/helper.h"
#include "hooking/byte_pattern.h"
#include "hooking/injector/calling.hpp"
#include "hooking/injector/hooking.hpp"
#include "hooking/injector/utility.hpp"

#include <WinUser.h>
#include <Windows.h>
#include <cstdint>
#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
std::intptr_t display3d_base;
WNDPROC       ffo_wndproc;

static injector::hook_back<int(__fastcall *)(std::intptr_t, int, HWND, int)> Display3D_Init_Hookback;
static injector::hook_back<int(__fastcall *)(std::intptr_t)>                 Display3D_Update_Hookback;
static injector::hook_back<int(__fastcall *)(std::intptr_t)>                 Display3D_Destroy_Hookback;

wchar_t Param_To_WideChar(WPARAM wParam)
{
    wchar_t     wc;
    char        buffer[2];
    const char *pSrc = reinterpret_cast<const char *>(&wParam);
    buffer[0]        = pSrc[1];
    buffer[1]        = pSrc[0];
    MultiByteToWideChar(936, 0, buffer, 2, &wc, 1);
    return wc;
}

// ImGui消息处理
LRESULT WINAPI FFO_ImGui_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImGuiIO &io = ImGui::GetIO();

    io.MouseDrawCursor = io.WantCaptureMouse;

    bool processed = false;

    if (io.WantCaptureKeyboard)
    {
        if (msg == WM_CHAR && wParam >= 0xA0 && lParam == 1)
        {
            // 忽略被拆开的GB2312字节
            processed = true;
        }
        else if (msg == WM_IME_CHAR && wParam > 0xA000 && lParam == 1)
        {
            // 将完整的GB2312字符转为Unicode再投给ImGui
            io.AddInputCharacterUTF16(Param_To_WideChar(wParam));
            processed = true;
        }
    }

    if (!processed)
    {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        {
            return true;
        }
    }
    else
    {
        return 0;
    }

    if (io.WantCaptureMouse && msg == WM_LBUTTONDOWN)
    {
        return 0;
    }

    if (io.WantCaptureKeyboard && msg == WM_CHAR)
    {
        return 0;
    }

    // 魔手模拟按键：把自定义消息 WM_APP + 0x100 转换为 WM_KEYUP 交给游戏
    // wParam 即虚拟键码(VK_F1...)，需放在 is_fkey 拦截之前，避免被"忽略松开"分支吃掉
    if (msg == WM_APP + 0x100)
    {
        return ffo_wndproc(hWnd, WM_KEYUP, wParam, lParam);
    }

    bool is_fkey   = (wParam >= VK_F1 && wParam <= VK_F10);
    bool is_sys    = (msg == WM_SYSKEYDOWN) || (msg == WM_SYSKEYUP);
    // 主键盘数字键('0'-'9')，仅在Alt组合(系统键消息)时拦截，不影响单独按数字键
    bool is_altnum = is_sys && (wParam >= '0' && wParam <= '9');

    if (is_fkey || is_altnum)
    {
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        {
            // lParam bit30(previous key state) 为1表示按住产生的自动重复
            bool is_repeat = (lParam & (1 << 30)) != 0;

            if (!is_repeat)
            {
                // 将首次按下变成松开（保持原消息系列）
                msg = is_sys ? WM_SYSKEYUP : WM_KEYUP;
            }
            else
            {
                // 忽略按住不放产生的重复
                return 0;
            }
        }
        else
        {
            // 忽略松开(WM_KEYUP / WM_SYSKEYUP)
            return 0;
        }
    }

    return ffo_wndproc(hWnd, msg, wParam, lParam);
}

// ImGui初始化
int __fastcall FFO_ImGui_Init(std::intptr_t display3d, int, HWND a0, int a4)
{
    auto game_init_result = Display3D_Init_Hookback.fun(display3d, 0, a0, a4);

    if (game_init_result == 0)
    {
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGui_ImplWin32_InitForOpenGL(a0);
        ImGui_ImplOpenGL2_Init();

        ImGuiIO &io = ImGui::GetIO();

        // 是的，C盘
        ImFont *font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr,
                                                    io.Fonts->GetGlyphRangesChineseFull());

        SetWindowLongPtrA(a0, GWLP_WNDPROC, reinterpret_cast<LONG>(&FFO_ImGui_WndProc));
    }

    return game_init_result;
}

// ImGui绘制过程
int __fastcall FFO_ImGui_Update(std::intptr_t display3d)
{
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    helper_instance.imgui_process();

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    return Display3D_Update_Hookback.fun(display3d);
}

// 销毁ImGui
int __fastcall FFO_ImGui_Destroy(std::intptr_t display3d)
{
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    return Display3D_Destroy_Hookback.fun(display3d);
}

// ============ FFO 合成键去抖补丁(运行时内存 patch，不改 exe 文件) ============
// Patch1 @0x49C081  75 14 -> 90 90   (NOP 掉 jnz，让合成 down 落入真实分支)
// Patch2 @0x49C050  75 1E -> EB 1E   (jnz 改 jmp，无条件放行合成 KEYUP)
// 不写死 VA：用锚点特征码扫描定位，写之前先校验原字节，避免打错版本打崩游戏。
// 结果通过 OutputDebugStringA 输出，用 DebugView 查看 "[ffo-patch] ..."。
enum class patch_kind
{
    nop2,    // 两字节 NOP（Patch1: 75 14 -> 90 90）
    jnz2jmp, // jnz(0x75) 改 jmp(0xEB)，保留后面的 rel8（Patch2: 75 1E -> EB 1E）
};

struct ffo_patch
{
    const char   *name;
    const char   *pattern;     // 锚点特征码；? 为通配（相对偏移/绝对地址处用 ?）
    std::intptr_t site_offset; // 补丁字节相对“匹配起始处”的偏移
    std::uint8_t  expect_op;   // 期望的原始首字节（校验用）
    std::uint8_t  expect_rel;  // 期望的原始第二字节（rel8，二次确认）
    patch_kind    kind;
};

// ⚠️ 这两条特征码是根据反汇编“反推”的编码，未在真实二进制上核对过。
//    若 DebugView 显示 matches!=1 或 bytes 不符，说明需要按你运行的版本重新取特征码。
constexpr ffo_patch kFfoPatches[] = {
    // 0x49C07A call sub_49BEE9 ; E8 ?? ?? ?? ??
    // 0x49C07F test al, al     ; 84 C0
    // 0x49C081 jnz  0x49C097   ; 75 14  <-- 目标，位于匹配起始 +7
    {"Patch1-down", "E8 ? ? ? ? 84 C0 75 14", 7, 0x75, 0x14, patch_kind::nop2},

    // 0x49C049 cmp [ecx+10Ch],0 ; 83 B9 0C 01 00 00 00
    // 0x49C050 jnz 0x49C070     ; 75 1E  <-- 目标，位于匹配起始 +7
    {"Patch2-keyup", "83 B9 0C 01 00 00 00 75 1E", 7, 0x75, 0x1E, patch_kind::jnz2jmp},
};

bool apply_one_patch(const ffo_patch &p)
{
    char         msg[256];
    byte_pattern patterner;
    patterner.find_pattern(p.pattern); // 默认扫主 exe(GetModuleHandleA(nullptr))

    if (!patterner.has_size(1)) // 必须唯一命中，0 个或多个都不打
    {
        wsprintfA(msg, "[ffo-patch] %s: matches=%u, skip\n", p.name, static_cast<unsigned>(patterner.count()));
        OutputDebugStringA(msg);
        return false;
    }

    std::intptr_t site = patterner.get(0).i(p.site_offset);
    std::uint8_t  op   = injector::ReadMemory<std::uint8_t>(site, true);
    std::uint8_t  rel  = injector::ReadMemory<std::uint8_t>(site + 1, true);

    if (op != p.expect_op || rel != p.expect_rel) // 原字节不符：版本不对/定位偏了/已打过
    {
        wsprintfA(msg, "[ffo-patch] %s: bytes %02X %02X != %02X %02X, skip\n", p.name, op, rel, p.expect_op,
                  p.expect_rel);
        OutputDebugStringA(msg);
        return false;
    }

    switch (p.kind)
    {
    case patch_kind::nop2:
        injector::MakeNOP(site, 2, true); // 90 90
        break;
    case patch_kind::jnz2jmp:
        injector::WriteMemory<std::uint8_t>(site, 0xEB, true); // 75 -> EB，rel8 不动
        break;
    }

    wsprintfA(msg, "[ffo-patch] %s: OK @ %p\n", p.name, reinterpret_cast<void *>(site));
    OutputDebugStringA(msg);
    return true;
}

void apply_ffo_debounce_patches()
{
    for (const auto &p : kFfoPatches)
    {
        apply_one_patch(p);
    }
}
} // namespace

void inject_game()
{
    byte_pattern patterner;

    auto display3d_module = GetModuleHandleW(L"Display3D.dll");
    display3d_base        = reinterpret_cast<std::intptr_t>(display3d_module);
    auto display3d_vtbl = reinterpret_cast<std::intptr_t>(GetProcAddress(display3d_module, "??_7IDisplay@@6B@")) + 0xDC;

    // 插入ImGui渲染
    injector::ReadObject(display3d_vtbl + 0xC, Display3D_Init_Hookback.fun);
    injector::WriteObject(display3d_vtbl + 0xC, &FFO_ImGui_Init, true);
    injector::WriteObject(display3d_vtbl + 0x10, &FFO_ImGui_Init, true);

    injector::ReadObject(display3d_vtbl + 0x24, Display3D_Update_Hookback.fun);
    injector::WriteObject(display3d_vtbl + 0x24, &FFO_ImGui_Update, true);

    injector::ReadObject(display3d_vtbl + 0x18, Display3D_Destroy_Hookback.fun);
    injector::WriteObject(display3d_vtbl + 0x18, &FFO_ImGui_Destroy, true);

    // 储存原始WndProc函数
    patterner.find_pattern("C7 45 A8 08 00 00 00 C7 45 AC");
    if (patterner.has_size(1))
    {
        ffo_wndproc = injector::ReadMemory<WNDPROC>(patterner.get(0).i(10));
    }

    // 运行时打去抖补丁（特征码定位 + 原字节校验，失败自动跳过）
    apply_ffo_debounce_patches();
}
