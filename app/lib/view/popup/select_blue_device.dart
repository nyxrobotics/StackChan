/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:convert';

import 'package:flutter/cupertino.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_svg/svg.dart';
import 'package:get/get.dart';
import 'package:stack_chan/util/XiaoZhi_util.dart';
import 'package:stack_chan/util/rsa_util.dart';
import 'package:stack_chan/view/popup/login_page.dart';

import '../../app_state.dart';
import '../../model/blue_device_info.dart';
import '../../model/blue_model.dart';
import '../../util/blue_util.dart';
import '../../util/mac_address_validator.dart';
import '../../util/value_constant.dart';
import 'device_name_page.dart';

enum DeviceWifiConfigStep { selectDevice, wifiConfig }

///scanBluetoothbinddevicepopup
class SelectBlueDevice extends StatefulWidget {
  const SelectBlueDevice({super.key});

  @override
  State<StatefulWidget> createState() => _SelectBlueDeviceState();
}

class _SelectBlueDeviceState extends State<SelectBlueDevice> {
  //connecttimeouttimer(preventNoWait)
  Timer? _connectTimer;

  //verifytimeouttimer(preventdeviceNoresponsewhen1to)
  Timer? _verifyTimer;

  //timeouttime:30Second(s)
  static const int _connectTimeout = 30;

  //verifytimeouttime:20Second(s)
  static const int _verifyTimeout = 20;

  bool _acceptBleCallbacks = true;
  BluetoothDevice? _verificationDevice;
  int? _verificationGeneration;
  BlueCharacteristicCallback? _characteristicCallback;
  BlueNotificationCallback? _notificationCallback;
  BlueReconnectCallback? _reconnectCallback;
  int _activationGeneration = 0;

  String? _getDeviceId(BlueDeviceInfo blueDeviceInfo) {
    final Map<int, List<int>> manufacturerDataMap =
        blueDeviceInfo.advertisementData[ValueConstant.manufacturerData];
    if (manufacturerDataMap.isNotEmpty) {
      final MapEntry<int, List<int>> firstEntry =
          manufacturerDataMap.entries.first;
      final List<int> customData = firstEntry.value;
      final address = customData.map((byte) {
        return byte.toRadixString(16).padLeft(2, '0').toUpperCase();
      }).join();
      return address;
    }
    return null;
  }

  RxString connectDeviceId = RxString("");

  @override
  void dispose() {
    final hadActiveBleSession =
        _verificationDevice != null || connectDeviceId.value.isNotEmpty;
    _acceptBleCallbacks = false;
    ++_activationGeneration;
    _verificationDevice = null;
    _verificationGeneration = null;
    _connectTimer?.cancel();
    _verifyTimer?.cancel();
    _unregisterBlueCallbacks();
    connectDeviceId.value = "";
    isSuccess = false;
    if (hadActiveBleSession) {
      unawaited(BlueUtil.shared.disconnectCurrentPeripheral());
    }
    super.dispose();
  }

  bool isSuccess = false;

  @override
  void initState() {
    super.initState();
    //enterbindStreamProcess / Thread,setbindflag
    _registerBlueCallbacks();
    _reconnectCallback = (device, generation) {
      if (_isActiveBleConnection(device, generation)) {
        AppState.shared.showToast("Reconnected to device successfully.");
      }
    };
    BlueUtil.shared.onReconnectSuccess = _reconnectCallback;
  }

  //resetconnectstate(System1wrap)
  void _resetConnectState() {
    connectDeviceId.value = "";
    _verificationDevice = null;
    _verificationGeneration = null;
    _connectTimer?.cancel();
    _verifyTimer?.cancel();
    if (mounted) {
      setState(() {});
    }
  }

  bool _isActiveBleConnection(BluetoothDevice device, int generation) {
    return _acceptBleCallbacks &&
        mounted &&
        BlueUtil.shared.isConnectionCurrent(device, generation);
  }

  void _unregisterBlueCallbacks() {
    if (identical(
      BlueUtil.shared.characteristicCallback,
      _characteristicCallback,
    )) {
      BlueUtil.shared.characteristicCallback = null;
    }
    if (identical(
      BlueUtil.shared.wifiSetCharacteristicCall,
      _notificationCallback,
    )) {
      BlueUtil.shared.wifiSetCharacteristicCall = null;
    }
    if (identical(BlueUtil.shared.onReconnectSuccess, _reconnectCallback)) {
      BlueUtil.shared.onReconnectSuccess = null;
    }
    _characteristicCallback = null;
    _notificationCallback = null;
    _reconnectCallback = null;
  }

  void _registerBlueCallbacks() {
    _characteristicCallback = (device, characteristic, generation) async {
      if (!_isActiveBleConnection(device, generation)) return;
      final properties = characteristic.properties;
      final canWrite = properties.write || properties.writeWithoutResponse;
      final uuid = characteristic.uuid.toString().toLowerCase();
      if (!canWrite ||
          uuid != BlueUtil.wifiSetCharacteristicUUID.toLowerCase()) {
        return;
      }

      _verificationDevice = device;
      _verificationGeneration = generation;

      if (!_isActiveBleConnection(device, generation)) return;
      await startVerificationEquipment(device, generation);
    };
    BlueUtil.shared.characteristicCallback = _characteristicCallback;

    _notificationCallback = (data) async {
      final device = _verificationDevice;
      final generation = _verificationGeneration;
      if (device == null ||
          generation == null ||
          !_isActiveBleConnection(device, generation)) {
        return;
      }

      try {
        final json = utf8.decode(data);
        final model = BlueNotifyStateModel.fromJson(json);

        if (model == null) {
          if (_isActiveBleConnection(device, generation)) {
            AppState.shared.showToast("Failed to parse device data.");
          }
          return;
        }

        if (model.cmd != null &&
            model.cmd == "notifyState" &&
            model.data?.type == 4) {
          //toEncryptinfo
          String? data = model.data?.state;
          if (data == null) {
            if (_isActiveBleConnection(device, generation)) {
              AppState.shared.showToast(
                "Device did not return encryption data.",
              );
            }
            return;
          }

          //RSADecrypt
          final result = RsaUtil.decryptStackChanBlue(data);
          //newIncrease / Add:Decryptfail/lengthNot
          if (result.isEmpty || result.length < 12) {
            if (_isActiveBleConnection(device, generation)) {
              AppState.shared.showToast(
                "Device verification decryption failed.",
              );
            }
            return;
          }

          if (!_isActiveBleConnection(device, generation)) return;
          if (isSuccess) return;
          isSuccess = true;
          _connectTimer?.cancel(); //cancelconnecttimeouttimer
          _verifyTimer?.cancel(); //cancelverifytimeouttimer

          final macAddress = result.substring(0, 12);

          AppState.shared.showToast(
            "The initially activated MAC address: $macAddress",
          );

          AppState.shared.deviceMac = macAddress;
          AppState.shared.connectWebSocket();
          final activationGeneration = ++_activationGeneration;
          unawaited(activateDevice(macAddress, activationGeneration));
        }
      } catch (e) {
        if (_isActiveBleConnection(device, generation)) {
          AppState.shared.showToast("Failed to process device data.");
        }
      }
    };
    BlueUtil.shared.wifiSetCharacteristicCall = _notificationCallback;
  }

  String formatMacAddress(String mac) {
    if (mac.length != 12) return mac;
    return mac
        .toUpperCase()
        .replaceAllMapped(RegExp(r'(.{2})'), (match) => '${match.group(1)}:')
        .substring(0, 17); //removeafter
  }

  bool isValidMac(String mac) {
    final RegExp macRegex = RegExp(r'^[0-9A-Fa-f]{12}$');
    return macRegex.hasMatch(mac);
  }

  //activatedevice
  bool _isActivationCurrent(int generation) {
    return _acceptBleCallbacks &&
        mounted &&
        generation == _activationGeneration;
  }

  Future<void> activateDevice(String macAddress, int generation) async {
    try {
      ///startqueryagentconfiginfo
      final isConfiguration = await queryConfiguration(macAddress, generation);
      if (!_isActivationCurrent(generation)) return;
      if (!isConfiguration) {
        _resetConnectState();
        //activatefail(Alreadyhashint,NoRepeat)
        return;
      }

      if (mounted) {
        AppState.shared.showToast("The AI Agent has been configured.");
      }

      //binddevice
      final result = await AppState.shared.bindDevice(macAddress);
      if (!_isActivationCurrent(generation)) return;
      if (!mounted) return;
      if (result) {
        _resetConnectState();
        AppState.shared.showToast("Device bound successfully!");
        //configjump
        Navigator.of(context).push(
          CupertinoPageRoute(builder: (context) => const DeviceNamePage()),
        );
      } else {
        _resetConnectState();
        //newIncrease / Add:binddevicefail
        AppState.shared.showToast("Device binding failed. Please try again.");
      }
    } catch (error, stackTrace) {
      debugPrint("Device activation failed: $error\n$stackTrace");
      if (!_isActivationCurrent(generation)) return;
      _resetConnectState();
      AppState.shared.showToast("Device activation exception.");
    }
  }

  //querydeviceconfig(StreamProcess / Threaderrorhint)
  Future<bool> queryConfiguration(String macAddress, int generation) async {
    try {
      //1. querydevicewhetheractivated (laterNotAgainquery directactivate)
      // final devices = await XiaoZhiUtil.shared.getDevice(macAddress);
      // if (devices.isNotEmpty) {
      //   if (devices.first.agent_id != null) {
      //debugPrint("✅ deviceactivated");
      //     return true;
      //   }
      // }

      //2. generatelicense
      final generateLicense = await XiaoZhiUtil.shared.generateLicense(
        macAddress,
      );
      if (!_isActivationCurrent(generation)) return false;
      if (generateLicense == null || generateLicense.serialNumber == null) {
        AppState.shared.showToast("Failed to generate device license.");
        return false;
      }

      //3. activatedevice
      final serialNumber = generateLicense.serialNumber!;
      final mac = MacAddressValidator.formatMac(macAddress);
      if (mac == null) {
        AppState.shared.showToast("Failed to format device MAC address.");
        return false;
      }
      bool activateResult = await XiaoZhiUtil.shared.agentsDevicesActivate(
        serialNumber,
        mac,
      );
      if (!_isActivationCurrent(generation)) return false;
      if (!activateResult) {
        AppState.shared.showToast("Device cloud activation failed.");
        return false;
      }

      ///deviceMayexist Notactivate,Needverify
      final checkDevice = await XiaoZhiUtil.shared.serialNumberGetDevice(
        serialNumber,
      );
      if (!_isActivationCurrent(generation)) return false;
      if (checkDevice == null || checkDevice.agent_id == null) {
        //activatefail
        return false;
      } else {
        AppState.shared.showToast("Device activation successful");
        return true;
      }
    } catch (e) {
      if (_isActivationCurrent(generation)) {
        AppState.shared.showToast("Failed to query device configuration.");
      }
      return false;
    }
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground.resolveFrom(
        context,
      ),
      child: CustomScrollView(
        slivers: [
          CupertinoSliverNavigationBar(
            largeTitle: Text("Select Device"),
            leading: SizedBox.shrink(),
            trailing: CupertinoButton(
              padding: .zero,
              child: Icon(
                CupertinoIcons.xmark_circle_fill,
                size: 25,
                color: CupertinoColors.separator.resolveFrom(context),
              ),
              onPressed: () {
                AppState.shared.manualShutdownTime = DateTime.now();
                _acceptBleCallbacks = false;
                ++_activationGeneration;
                _verificationDevice = null;
                _verificationGeneration = null;
                _connectTimer?.cancel();
                _verifyTimer?.cancel();
                _unregisterBlueCallbacks();
                unawaited(BlueUtil.shared.disconnectCurrentPeripheral());
                CupertinoSheetRoute.popSheet(context);
              },
            ),
          ),
          SliverList.list(
            children: [
              Obx(
                () => CupertinoListSection.insetGrouped(
                  children: AppState.shared.blueDeviceList.isNotEmpty
                      ? AppState.shared.blueDeviceList.map((deviceInfo) {
                          String? deviceId = _getDeviceId(deviceInfo);
                          return CupertinoListTile(
                            onTap: () async {
                              if (!AppState.shared.isLogin.value) {
                                if (mounted) {
                                  Navigator.of(context).push(
                                    CupertinoPageRoute(
                                      builder: (context) =>
                                          const LoginPage(isWelCome: true),
                                    ),
                                  );
                                }
                                return;
                              }

                              //Repeatclick:Alreadyinconnectin
                              final deviceRemoteId = deviceInfo.device.remoteId
                                  .toString();
                              if (deviceRemoteId == connectDeviceId.value) {
                                if (mounted) {
                                  AppState.shared.showToast(
                                    "Already connecting... Please wait patiently.",
                                  );
                                }
                                return;
                              }

                              //startconnect
                              isSuccess = false;
                              connectDeviceId.value = deviceRemoteId;
                              AppState.shared.blueDeviceList.refresh();

                              //newIncrease / Add:connecttimeouttimer
                              _connectTimer?.cancel();
                              _connectTimer = Timer(
                                Duration(seconds: _connectTimeout),
                                () {
                                  if (connectDeviceId.value != deviceRemoteId) {
                                    return;
                                  }
                                  _resetConnectState();
                                  unawaited(
                                    BlueUtil.shared
                                        .disconnectCurrentPeripheral(),
                                  );
                                  if (mounted) {
                                    AppState.shared.showToast(
                                      "Device connection timed out. Please try again.",
                                    );
                                  }
                                },
                              );

                              //newIncrease / Add:BluetoothconnectThrowsCatch
                              try {
                                if (mounted) {
                                  AppState.shared.showToast(
                                    "Connecting to device...",
                                  );
                                }
                                await BlueUtil.shared.connect(
                                  deviceInfo.device,
                                );
                              } catch (e) {
                                if (!_acceptBleCallbacks ||
                                    !mounted ||
                                    connectDeviceId.value != deviceRemoteId) {
                                  return;
                                }
                                _resetConnectState();
                                if (mounted) {
                                  AppState.shared.showToast(
                                    "Bluetooth connection failed: ${e.toString()}",
                                  );
                                }
                              }
                            },
                            leading: Image.asset(
                              "assets/image1.png",
                              width: 28,
                              height: 28,
                            ),
                            title: Text(deviceInfo.device.advName),
                            subtitle: deviceId != null
                                ? Text("ID: $deviceId")
                                : null,
                            trailing:
                                deviceInfo.device.remoteId.toString() ==
                                    connectDeviceId.value
                                ? const CupertinoActivityIndicator()
                                : SvgPicture.asset(
                                    "assets/chevron.right.svg",
                                    width: 15,
                                    height: 15,
                                    colorFilter: ColorFilter.mode(
                                      CupertinoColors.secondaryLabel
                                          .resolveFrom(context),
                                      BlendMode.srcIn,
                                    ),
                                  ),
                          );
                        }).toList()
                      : [CupertinoListTile(title: Text("No devices found"))],
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }

  //startverifydevice(senddata)
  Future<void> startVerificationEquipment(
    BluetoothDevice device,
    int generation,
  ) async {
    if (!_isActiveBleConnection(device, generation)) return;

    try {
      AppState.shared.showToast("Verifying device...");
      //startverifytimeouttimer
      _verifyTimer?.cancel();
      _verifyTimer = Timer(Duration(seconds: _verifyTimeout), () {
        if (!_isActiveBleConnection(device, generation)) return;
        _resetConnectState();
        unawaited(BlueUtil.shared.disconnectCurrentPeripheral());
        if (mounted) {
          AppState.shared.showToast(
            "Device verification timed out. Please try again.",
          );
        }
      });
      //data
      final dateTimeString = DateTime.now().millisecondsSinceEpoch.toString();
      final BlueEncryptionDecryption data = BlueEncryptionDecryption(
        cmd: "handshake",
        data: dateTimeString,
      );
      final jsonString = jsonEncode(data.toJson());

      //senddata
      final result = await BlueUtil.shared.sendWifiSetData(
        jsonString,
        expectedDevice: device,
        connectionGeneration: generation,
      );
      if (!_isActiveBleConnection(device, generation)) return;
      if (!result) {
        _verifyTimer?.cancel();
        throw StateError(
          "The equipment may have been disconnected. "
          "Please reconfigure it on the StackChan end.",
        );
      }
    } catch (error, stackTrace) {
      if (!_isActiveBleConnection(device, generation)) return;
      _verifyTimer?.cancel();
      Error.throwWithStackTrace(error, stackTrace);
    }
  }
}
