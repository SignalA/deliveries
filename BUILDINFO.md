# Build information

Version:0.2.10(2026-09-20)
Target:A40i ARMv7 GNU EABI softfp,/lib/ld-linux.so.3.
Compiler:Linaro GCC 7.3.1 arm-linux-gnueabi.
Sources:src/;rebuild with Makefile and ARM compiler.
Validation:16 protocol tests,evdev unit tests,multibutton UDS tests and calibration HID tests passed on 2026-09-22.Live board testing confirmed calibration-role delivery of A/COIN DOWN/UP and one GUN_RESET per disconnected gun.

- 2026-09-22：修复校准应用发送CALIB_RESULT后立即退出时，服务端提前移除客户端导致0x06未发送的问题。
- 校准采样坐标和目标坐标统一按0..32767校验。
- 新增客户端发完结果立即关闭套接字的回归测试。

- 最终实机验证：index=0x07成功，8个index=0x06完整发送，0x0A结果可接收，index=0x08状态可解析。
- 插件不再拦截坐标相同的校准数据，质量判断交由枪端固件。
- HELLO calibration客户端发完CALIB_RESULT立即退出时，服务端仍会先处理已排队结果。

- 2026-09-22最终实测：68字段结果接收正常，8个0x06完整发送，枪返回0x0A=0；index=0x08返回质量拒绝由枪端数据决定。
- 修复新版校准应用在68字段结果中填写count=32时被插件当作坏消息的问题；现在兼容count=32和早期count=24。实机确认已进入index=0x07、8个index=0x06及index=0x08完整链路。
