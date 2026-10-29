端到端测试 (可选, 仅开发时使用)
================================

这些脚本用本地 MQTT Broker (amqtt) + 模拟 PBX (paho-mqtt) 在没有真机的情况下
验证拨号器的完整流程, 不参与打包, 拷到现场也不需要它们。

依赖
----
Python 3 + paho-mqtt + amqtt。旧工程 pbx-dialer\testlibs 里有一份现成的,
把它加入 PYTHONPATH 即可 (脚本里默认找 ..\testlibs)。

跑法
----
1) 起 Broker:      python run_broker.py            (监听 127.0.0.1:1883)
2) 起模拟 PBX:     python fake_pbx.py              (token=testtoken001)
3) 逐个跑:         python e2e_dial.py  <exe 的绝对路径>
                   python e2e_caller.py <exe 的绝对路径>
                   python e2e_extlist.py <exe 的绝对路径>
                   python e2e_ini.py <exe 的绝对路径>
                   python e2e_diag.py <exe 的绝对路径>
                   python e2e_autoanswer.py <exe 的绝对路径>
                   python e2e_ui.py        <exe 的绝对路径>   (界面精简/回车/双击历史)

注意
----
- exe 路径要给绝对路径 (脚本会用它的目录作为 ini 位置)。
- 每个脚本结束会打印 "== 结果: N/M ==", M 与 N 相等即全部通过。
- 当前合计 71 项: dial 9 + caller 14 + extlist 7 + ini 5 + diag 5 + autoanswer 12 + ui 19。
- e2e_diag 需要 show_diag=1 (脚本自己会写); 默认界面上没有 [诊断] 按钮。
- 测试会自己写 pbxdialer.ini, 跑完可以删掉。
- 运行前脚本会先 taskkill 同名进程 (单实例保护会让新进程直接退出)。