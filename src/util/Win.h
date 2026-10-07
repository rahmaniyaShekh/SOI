#pragma once
//
// Minimal RAII wrappers for the Win32/COM handles this project touches.
// Deliberately small: only the operations actually used are exposed, so there is
// no ambiguity about ownership at call sites.
//
#include "util/Platform.h"

#include <windows.h>
#include <string>
#include <utility>

namespace soi {

// ---------------------------------------------------------------------------
// COM smart pointer. Intentionally not ATL/WRL so the project has no extra
// toolchain requirements.
// ---------------------------------------------------------------------------
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}

    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { if (o.p_) o.p_->AddRef(); reset(); p_ = o.p_; }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = std::exchange(o.p_, nullptr); }
        return *this;
    }

    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }

    T*  get()  const { return p_; }
    T*  operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // For [out] parameters: releases any current value first.
    T** put() { reset(); return &p_; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p_); }

    // Transfers ownership of an already-AddRef'd pointer.
    void attach(T* raw) { reset(); p_ = raw; }
    T*   detach() { return std::exchange(p_, nullptr); }

    template <class U>
    HRESULT as(ComPtr<U>& out) const {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(__uuidof(U), out.putVoid());
    }

private:
    T* p_ = nullptr;
};

// ---------------------------------------------------------------------------
// GDI handles. GDI leaks are silent and eventually fatal (10k object limit per
// process), so every allocation in the capture path goes through these.
// ---------------------------------------------------------------------------
template <class H, BOOL(WINAPI* Deleter)(H)>
class GdiObj {
public:
    GdiObj() = default;
    explicit GdiObj(H h) : h_(h) {}
    ~GdiObj() { reset(); }

    GdiObj(const GdiObj&) = delete;
    GdiObj& operator=(const GdiObj&) = delete;
    GdiObj(GdiObj&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    GdiObj& operator=(GdiObj&& o) noexcept {
        if (this != &o) { reset(); h_ = std::exchange(o.h_, nullptr); }
        return *this;
    }

    void reset(H h = nullptr) { if (h_) Deleter(h_); h_ = h; }
    H    get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    H h_ = nullptr;
};

inline BOOL WINAPI deleteDcShim(HDC dc)          { return DeleteDC(dc); }
inline BOOL WINAPI deleteBitmapShim(HBITMAP b)   { return DeleteObject(b); }

using ScopedDc     = GdiObj<HDC, deleteDcShim>;
using ScopedBitmap = GdiObj<HBITMAP, deleteBitmapShim>;

// A DC obtained from GetDC/GetWindowDC must be released, not deleted.
class WindowDc {
public:
    WindowDc() = default;
    WindowDc(HWND wnd, HDC dc) : wnd_(wnd), dc_(dc) {}
    ~WindowDc() { if (dc_) ReleaseDC(wnd_, dc_); }

    WindowDc(const WindowDc&) = delete;
    WindowDc& operator=(const WindowDc&) = delete;
    WindowDc(WindowDc&& o) noexcept
        : wnd_(std::exchange(o.wnd_, nullptr)), dc_(std::exchange(o.dc_, nullptr)) {}
    WindowDc& operator=(WindowDc&& o) noexcept {
        if (this != &o) {
            if (dc_) ReleaseDC(wnd_, dc_);
            wnd_ = std::exchange(o.wnd_, nullptr);
            dc_  = std::exchange(o.dc_, nullptr);
        }
        return *this;
    }

    HDC get() const { return dc_; }
    explicit operator bool() const { return dc_ != nullptr; }

private:
    HWND wnd_ = nullptr;
    HDC  dc_  = nullptr;
};

// Restores whatever object was selected into a DC on scope exit.
class SelectGuard {
public:
    SelectGuard(HDC dc, HGDIOBJ obj) : dc_(dc), prev_(SelectObject(dc, obj)) {}
    ~SelectGuard() { if (prev_) SelectObject(dc_, prev_); }
    SelectGuard(const SelectGuard&) = delete;
    SelectGuard& operator=(const SelectGuard&) = delete;
private:
    HDC     dc_;
    HGDIOBJ prev_;
};

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16. The whole project speaks UTF-8 internally; only the Win32
// boundary uses wide strings.
// ---------------------------------------------------------------------------
inline std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring toUtf16(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

} // namespace soi
