# Changelog

## v0.1.3 (2026-04-29)

### Changed

- 版本号更新为 `v0.1.3`
- HUD 从 GDI 固定绘制升级为 Direct2D/DirectWrite 渲染
- HUD 尺寸改为 DPI-aware 的 DIP 计算，按实际文本宽度动态调整
- 5 根录音音量条改为由实时 PCM RMS 驱动，并调大可视尺寸
- 构建链接同步加入 `d2d1.lib` / `dwrite.lib`

### Fixed

- 修复高 DPI 下 HUD 文本被裁切的问题
- 修复 HUD 文本垂直居中不稳定的问题
- 修复 layered window 圆角边缘可能出现黑边的问题

## v0.1.2 (2026-04-29)

### Changed

- 托盘菜单版本号更新为 `v0.1.2`
- 默认 `CapsLock` 快捷键改为 300ms 长按触发语音输入
- 短按 `CapsLock` 交还系统处理，用于正常切换大小写

### Fixed

- 长按 `CapsLock` 语音输入结束后恢复按下前的大小写状态，避免误切换 Caps Lock
- 补发短按 `CapsLock` 时放行注入事件，避免被全局键盘 hook 再次拦截

## v0.1.1 (2026-04-29)

### Changed

- 线程上限从 4 提升至 8，auto 策略改为 `min(8, cpu_count)`
- Settings 线程选项从 1/2/3/4/auto 扩展为 1..8/auto，auto 项显示实际线程数

### Fixed

- Settings 窗口打开时固定在屏幕中央，不再出现在左上角

## v0.1.0 (2026-04-28)

- Initial release
