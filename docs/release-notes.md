# 薄荷桌面助手 / MintDesk v0.8.2

| 更新 | 效果 |
| --- | --- |
| 卡片层级 | 修复偶发盖住普通窗口；前台不变的窗口重排、毛玻璃窗口独立置顶后也会校正 |
| 桌面切换 | Explorer 宿主暂不可用时安全退到底层，桌面视图恢复后重新定位 |
| 低占用 | 使用合并后的事件通知校正层级，不增加持续轮询或周期重绘 |
| EXE 换目录 | 从新位置正常打开后，刷新自有快捷方式及仍启用的自启动、右键路径；保留手动关闭选择 |

EXE **416 KiB**，无需额外依赖或运行库。Windows 10 / 11 x64 · MIT · © 2026 MCC。

**更新请先正常退出旧版，再覆盖原位置的 `MintDesk.exe` 并打开。** 用户 AppData 中的布局、顺序和恢复记录继续沿用。桌面快捷方式依赖原 EXE，不能删除程序本体；换目录时先退出、移动，再从新位置打开一次。

提供便携 ZIP、独立 EXE 和 SHA-256 校验。Windows 可能对未签名程序显示安全提示；受保护项目可能需要管理员确认。详见[使用说明](https://github.com/mayflyfy/mint-desk/blob/main/docs/usage.md)与[验证范围](https://github.com/mayflyfy/mint-desk/blob/main/docs/verification.md)。
