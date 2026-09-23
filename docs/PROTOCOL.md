# UDS接入协议

AF_UNIX / SOCK_DGRAM；每条消息一个数据报，文本以换行结尾。客户端须绑定独立绝对路径，与服务端同用户运行。

- 游戏握手：HELLO game 1，返回HELLO_OK 1。
- 校准握手：HELLO calibration 1（兼容2），返回对应HELLO_OK。
- 校准输入：GUN_DOWN <player> <x> <y>；这里player是游戏映射编号（默认1/2），与结果里的档案编号不同。对方当前应用按编号减1匹配 --gun，因此联调保持默认 --player1 1 --player2 2。
- 游戏输入：GUN_MOVE/GUN_DOWN/GUN_UP <player> <x> <y>，GUN_BUTTON <player> A|B|COIN|PAUSE DOWN|UP，GUN_RESET <player>。
- 坐标默认0..32767。校准期间暂停游戏输入，不发送GUN_MOVE给校准应用。
- 服务端每秒PING 1；客户端应回复PONG 1。客户端PING 1会收到PONG 1。

结果格式：

```text
CALIB_RESULT <gun> <player> 32 <x0> <y0> <x1> <y1> <x2> <y2> <target_x0> <target_y0> ...
```

gun=0/1表示物理枪1/2；player=0..3表示枪内档案。count=32表示24组实测坐标加8组目标坐标。每个目标依次放3组实测坐标和1组目标坐标，共8个目标、68个文本字段（含CALIB_RESULT）。坐标和目标点均为0..32767。插件兼容早期应用在相同68字段布局中填写count=24。
第一把扣枪锁定会话，结果gun必须匹配；另一个档案或另一把枪请使用新校准连接。旧51/67字段结果仍接受，不建议新版应用使用。

真实写入确认：CALIB_STORED 1 APPLIED。模拟模式：CALIB_STORED 1 CAPTURE_ONLY。
失败返回ERROR BAD_CALIB_RESULT、CALIB_DEVICE_NOT_FOUND、CALIB_WRITE_FAILED、CALIB_NO_RESPONSE、CALIB_DEVICE_REJECTED或SAVE_FAILED。
真实写入要求枪固件支持随包HOST_INTEGRATION_GUIDE.md的index=7/8。

## 游戏切换枪的校准档案

游戏客户端先发送 `HELLO game 1`，再发送 `SET_PROFILE <gunNum> <gameCurPnum>`。gunNum为0/1，对应物理枪1/2；gameCurPnum为0..3，对应枪内四个校准档案。两者都从0开始。一个客户端可切换任意一把枪。

插件向该枪的管理HID发送index=7 选择档案，等待成功回包，再发送index=8 查询并核对当前档案。成功回复 `PROFILE_SET <gunNum> <gameCurPnum>`。失败回复 `ERROR GAME_ROLE_REQUIRED`、`BAD_SET_PROFILE`、`CALIBRATION_BUSY`、`PROFILE_DEVICE_NOT_FOUND`、`PROFILE_WRITE_FAILED`、`PROFILE_NO_RESPONSE` 或 `PROFILE_DEVICE_REJECTED`（ERROR后只有一个错误码）。校准连接活跃时不执行切档。

示例：游戏发 `SET_PROFILE 0 2`，成功收到 `PROFILE_SET 0 2`，表示枪1已切到档案2。档案可以为空，切档成功不代表该档已标定。
