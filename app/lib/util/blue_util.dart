/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/cupertino.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:stack_chan/app_state.dart';
import 'package:stack_chan/model/expression_data.dart';
import 'package:stack_chan/util/value_constant.dart';
import 'package:stack_chan/view/app.dart';
import 'package:stack_chan/view/popup/device_wifi_config.dart';

import '../model/blue_device_info.dart';
import '../model/dance_list.dart';

typedef BlueCharacteristicCallback =
    FutureOr<void> Function(
      BluetoothDevice device,
      BluetoothCharacteristic characteristic,
      int connectionGeneration,
    );
typedef BlueReconnectCallback =
    void Function(BluetoothDevice device, int connectionGeneration);
typedef BlueNotificationCallback = FutureOr<void> Function(List<int> value);

class _PendingDanceWrite {
  _PendingDanceWrite({
    required this.peripheral,
    required this.generation,
    required this.motionPayload,
    required this.avatarPayload,
    required this.rgbPayload,
  });

  final BluetoothDevice peripheral;
  final int generation;
  final String motionPayload;
  final String avatarPayload;
  final String rgbPayload;
  final Completer<bool> completer = Completer<bool>();
}

class BlueUtil {
  static final BlueUtil shared = BlueUtil._internal();

  BlueUtil._internal() {
    _initialize();
  }

  static const String danceTargetServiceUUID =
      "e2e5e5e0-1234-5678-1234-56789abcdef0";

  //MARK: - Core constants (align with iOS)
  static const String targetServiceUUID =
      "e2e5e5ff-1234-5678-1234-56789abcdef0";
  static const String headCharacteristicUUID =
      "0000ffe1-0000-1000-8000-00805f9b34fb";
  static const String wifiSetCharacteristicUUID =
      "e2e5e5e3-1234-5678-1234-56789abcdef0";
  static const String expressionCharacteristicUUID =
      "0000ffe3-0000-1000-8000-00805f9b34fb";
  static const String writeCharacteristicUUID =
      "0000ffe4-0000-1000-8000-00805f9b34fb";

  //MARK: - Core properties (align with iOS)
  List<BlueDeviceInfo> discoveredDevices = [];
  bool blueSwitch = false;
  final bool autoReconnect = true;
  BluetoothDevice? currentPeripheral;

  //Auto scan enabled by default
  bool automaticScanning = true;

  //Feature object
  BluetoothCharacteristic? writeCharacteristic;
  BluetoothCharacteristic? writeExpressionCharacteristic;
  BluetoothCharacteristic? writeHeadCharacteristic;
  BluetoothCharacteristic? writeWifiSetCharacteristic;

  //MARK: - Callback closures
  Function(List<BlueDeviceInfo>)? blufDevicesMonitoring;
  Function(BluetoothAdapterState)? centralManagerDidUpdateState;
  BlueCharacteristicCallback? characteristicCallback;
  Function(BluetoothDevice, bool)? connectionStateChanged;
  BlueNotificationCallback? wifiSetCharacteristicCall;

  //MARK: - Private properties
  StreamSubscription? _scanSubscription;
  StreamSubscription<BluetoothAdapterState>? _adapterStateSubscription;
  StreamSubscription<BluetoothConnectionState>? _connectionStateSubscription;
  final Map<int, List<StreamSubscription<List<int>>>>
  _characteristicSubscriptions = {};
  Timer? _cleanupTimer;
  Timer? _reconnectTimer;
  final Duration _deviceTimeout = const Duration(seconds: 3);
  bool _isStartingScan = false;
  bool _isCheckingDeviceChanges = false;
  bool _manualDisconnect = false;
  bool _disposed = false;
  int _connectionGeneration = 0;
  int? _connectingGeneration;
  int? _connectedGeneration;
  int? _readyGeneration;
  int? _discoveryGeneration;
  int _connectRequestGeneration = 0;
  Future<void>? _discoveryFuture;
  Future<bool>? _reconnectFuture;
  Future<void> _connectTail = Future<void>.value();
  Future<void>? _disconnectFuture;
  Future<void>? _disposeFuture;
  BluetoothDevice? _lastPeripheral;
  int _reconnectAttempt = 0;
  bool _danceWriteActive = false;
  _PendingDanceWrite? _pendingDanceWrite;
  String? _lastMotionPayload;
  String? _lastAvatarPayload;
  String? _lastRgbPayload;
  DateTime? _lastDanceWriteError;

  static const List<Duration> _reconnectDelays = [
    Duration(milliseconds: 300),
    Duration(seconds: 1),
    Duration(seconds: 2),
    Duration(seconds: 4),
    Duration(seconds: 8),
  ];
  static const Duration _subscriptionCancelTimeout = Duration(seconds: 3);
  static const Duration _scanOperationTimeout = Duration(seconds: 5);
  static const int _writeTimeoutSeconds = 5;

  static const String motionCharacteristicUUID =
      "e2e5e5e1-1234-5678-1234-56789abcdef0";
  static const String avatarCharacteristicUUID =
      "e2e5e5e2-1234-5678-1234-56789abcdef0";
  static const String configCharacteristicUUID =
      "e2e5e5e3-1234-5678-1234-56789abcdef0";
  static const String rgbCharacteristicUUID =
      "e2e5e5e4-1234-5678-1234-56789abcdef0";
  BluetoothCharacteristic? writeMotionCharacteristic;
  BluetoothCharacteristic? writeAvatarCharacteristic;
  BluetoothCharacteristic? writeRGBCharacteristic;

  int blueMode = 1; //1 WiFi mode  2 Dance mode 3 Pairing mode

  ///Cache scanned device list for change comparison
  List<String> cachedDeviceMacs = [];

  //MARK: - Initialization (fix Android first timing issue: permission → listen → enable Bluetooth)
  void _initialize() {
    //[Fix] Request permission first, init listener and Bluetooth after permission granted
    unawaited(_requestBluetoothPermissions());
  }

  //MARK: - Request Bluetooth permission (permission_handler)
  Future<void> _requestBluetoothPermissions() async {
    try {
      if (Platform.isAndroid) {
        //Android 12+ permissions
        await [
          Permission.bluetooth,
          Permission.bluetoothScan,
          Permission.bluetoothConnect,
          Permission.location,
        ].request();
      } else if (Platform.isIOS) {
        //iOS permissions
        await [
          Permission.bluetooth,
          Permission.bluetoothScan,
          Permission.bluetoothConnect,
          Permission.location,
        ].request();
      } else {
        return;
      }
    } catch (e) {
      debugPrint("Failed to request Bluetooth permissions: $e");
    }

    if (_disposed) return;

    // Continue observing the adapter even after a denied permission so a
    // later permission change can recover without recreating this singleton.
    try {
      await _registerBluetoothStateListener();
      await _tryTurnOnBluetooth();
    } catch (e) {
      debugPrint("Failed to initialize Bluetooth: $e");
    }
  }

  Future<void> _registerBluetoothStateListener() async {
    if (_disposed) return;

    final previousSubscription = _adapterStateSubscription;
    _adapterStateSubscription = null;
    await _cancelSubscription(previousSubscription, "Bluetooth adapter state");
    if (_disposed) return;

    _adapterStateSubscription = FlutterBluePlus.adapterState.listen(
      (state) {
        if (_disposed) return;
        try {
          centralManagerDidUpdateState?.call(state);
        } catch (e) {
          debugPrint("Bluetooth adapter callback failed: $e");
        }
        _centralManagerDidUpdateState(state);
      },
      onError: (Object error) {
        if (!_disposed) {
          debugPrint("Bluetooth adapter state stream failed: $error");
        }
      },
    );
  }

  //MARK: - [New] Auto enable Bluetooth
  Future<void> _tryTurnOnBluetooth() async {
    if (_disposed) return;
    try {
      final currentState = FlutterBluePlus.adapterStateNow;

      if (currentState == BluetoothAdapterState.off) {
        await FlutterBluePlus.turnOn();
      } else if (currentState == BluetoothAdapterState.on) {
        //[fixkey]Androidfirstpermissionsuccess+BluetoothAlreadyenable → proactivetriggerscan
        blueSwitch = true;
        if (automaticScanning) {
          unawaited(startScan());
        }
        if (autoReconnect) {
          unawaited(reconnect());
        }
      }
    } catch (e) {
      debugPrint("Failed to update Bluetooth adapter state: $e");
    }
  }

  //MARK: - Bluetooth status update (auto scan, auto reconnect)
  void _centralManagerDidUpdateState(BluetoothAdapterState state) {
    switch (state) {
      case BluetoothAdapterState.unknown:
        break;
      case BluetoothAdapterState.unavailable:
        break;
      case BluetoothAdapterState.unauthorized:
        break;
      case BluetoothAdapterState.turningOn:
        break;
      case BluetoothAdapterState.on:
        blueSwitch = true;
        //Bluetoothenable autostartscan
        if (automaticScanning) {
          unawaited(startScan());
        }
        //autoreconnect
        if (autoReconnect) {
          unawaited(reconnect());
        }
        break;
      case BluetoothAdapterState.turningOff:
        break;
      case BluetoothAdapterState.off:
        blueSwitch = false;
        _reconnectTimer?.cancel();
        _reconnectTimer = null;
        //closeafterautoTryreOpen
        unawaited(_tryTurnOnBluetooth());
        break;
    }
  }

  //MARK: - Scan related (auto execute)
  Future<void> startScan() async {
    if (_disposed || _isStartingScan) {
      return;
    }

    _isStartingScan = true;
    try {
      if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
        //stateNotMeet / Satisfy,autoOpenBluetooth
        await _tryTurnOnBluetooth();
        return;
      }

      final previousSubscription = _scanSubscription;
      _scanSubscription = null;
      await _cancelSubscription(previousSubscription, "Bluetooth scan");
      await FlutterBluePlus.stopScan().timeout(_scanOperationTimeout);
      if (_disposed) return;

      discoveredDevices.clear();
      await FlutterBluePlus.startScan(
        withServices: [Guid(targetServiceUUID), Guid(danceTargetServiceUUID)],
        continuousUpdates: true,
        removeIfGone: _deviceTimeout,
      ).timeout(_scanOperationTimeout);
      if (_disposed) {
        await FlutterBluePlus.stopScan().timeout(_scanOperationTimeout);
        return;
      }

      _scanSubscription = FlutterBluePlus.scanResults.listen(
        (results) {
          if (_disposed) return;
          for (var result in results) {
            try {
              _centralManagerDidDiscoverPeripheral(result);
            } catch (e) {
              debugPrint("Failed to process Bluetooth scan result: $e");
            }
          }
        },
        onError: (Object error) {
          debugPrint("Bluetooth scan stream failed: $error");
        },
      );

      _startCleanupTimer();
    } catch (e) {
      debugPrint("Failed to start Bluetooth scan: $e");
    } finally {
      _isStartingScan = false;
    }
  }

  //Corresponds to iOS centralManager didDiscover peripheral method
  void _centralManagerDidDiscoverPeripheral(ScanResult result) {
    final advertisementDataMap = {
      ValueConstant.advName: result.advertisementData.advName,
      ValueConstant.txPowerLevel: result.advertisementData.txPowerLevel,
      ValueConstant.connectable: result.advertisementData.connectable,
      ValueConstant.serviceUuids: result.advertisementData.serviceUuids
          .map((g) => g.toString())
          .toList(),
      ValueConstant.serviceData: result.advertisementData.serviceData.map(
        (k, v) => MapEntry(k.toString(), v),
      ),
      ValueConstant.manufacturerData: result.advertisementData.manufacturerData,
    };

    final deviceInfo = BlueDeviceInfo(
      device: result.device,
      advertisementData: advertisementDataMap,
      rssi: result.rssi,
      lastSeen: DateTime.now(),
    );

    final index = discoveredDevices.indexWhere(
      (d) => d.device.remoteId == result.device.remoteId,
    );
    if (index == -1) {
      discoveredDevices.add(deviceInfo);
    } else {
      discoveredDevices[index] = deviceInfo;
    }

    //[Fix]WhenbounddeviceCurrentlyinconnectorconnectedwhen,NotexecutedevicediscoverStreamProcess / Thread
    if (currentPeripheral != null) {
      return;
    }

    //Determine behavior based on blueMode
    switch (blueMode) {
      case 1:
        //Change WiFi mode: Check device changes, auto connect bound devices
        unawaited(_checkDeviceChanges());
        break;
      case 2:
        //Dance mode: Only connect own device (requires deviceControlMode == 1)
        if (AppState.shared.deviceControlMode == 1) {
          screenMyDevice(discoveredDevices);
        }
        break;
      case 3:
        //pairingmode:callbackdevicelistFor / ToUIshow
        _notifyDiscoveredDevices();
        break;
    }
  }

  ///checkdevicelistchange，hasnewdevicewhencheckwhetherbound
  Future<void> _checkDeviceChanges() async {
    if (_isCheckingDeviceChanges ||
        (_reconnectTimer?.isActive ?? false) ||
        _reconnectFuture != null) {
      return;
    }

    final newDate = DateTime.now();
    if (AppState.shared.manualShutdownTime != null) {
      final Duration difference = newDate.difference(
        AppState.shared.manualShutdownTime!,
      );
      if (difference.inSeconds < 6) {
        return;
      }
    }

    _isCheckingDeviceChanges = true;
    try {
      //Get MAC addresses of all current devices
      List<String> currentMacs = [];
      for (final device in discoveredDevices) {
        final mac = _getDeviceId(device);
        if (mac != null) {
          currentMacs.add(mac.toUpperCase());
        }
      }
      //comparewhetherhasnewdeviceAppear / Occur
      bool hasNewDevice = false;
      for (final mac in currentMacs) {
        if (!cachedDeviceMacs.contains(mac)) {
          hasNewDevice = true;
          break;
        }
      }

      //updatecache
      cachedDeviceMacs = currentMacs;
      //ifhasnewdevice,AnduserAlreadylogin,checkwhetherisbounddevice
      if (hasNewDevice && AppState.shared.isLogin.value) {
        //Getbounddevicelist
        await AppState.shared.getDevices();

        //checkcurrentscantodevicewhetherinboundlistin
        for (final deviceInfo in discoveredDevices) {
          final String? deviceMac = _getDeviceId(deviceInfo);
          if (deviceMac == null) continue;

          final upperMac = deviceMac.toUpperCase();
          //checkwhetherisbounddevice
          final isBound = AppState.shared.devices.any(
            (device) => device.mac.toUpperCase() == upperMac,
          );

          if (isBound && currentPeripheral == null) {
            try {
              await connect(deviceInfo.device, automatic: true);
            } catch (e) {
              debugPrint("Failed to connect discovered Bluetooth device: $e");
              continue;
            }
            if (AppState.shared.popupState) {
              return;
            }
            AppState.shared.popupState = true;
            await showCupertinoSheet(
              context: App.appContext(),
              builder: (context) {
                return DeviceWifiConfig();
              },
            );
            AppState.shared.popupState = false;
            break;
          }
        }
      }
    } catch (e) {
      debugPrint("Failed to process discovered Bluetooth devices: $e");
    } finally {
      _isCheckingDeviceChanges = false;
    }
  }

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

  void screenMyDevice(List<BlueDeviceInfo> devices) {
    if (AppState.shared.deviceMac.isEmpty ||
        (_reconnectTimer?.isActive ?? false) ||
        _reconnectFuture != null ||
        _connectingGeneration != null) {
      return;
    }
    for (final deviceInfo in devices) {
      final String? deviceMac = _getDeviceId(deviceInfo);
      if (deviceMac == null) {
        continue;
      }
      final String targetMac = AppState.shared.deviceMac.toUpperCase();
      if (deviceMac.toUpperCase() == targetMac) {
        unawaited(_connectDiscoveredDevice(deviceInfo.device));
        break;
      }
    }
  }

  Future<void> _connectDiscoveredDevice(BluetoothDevice peripheral) async {
    try {
      await connect(peripheral, automatic: true);
    } catch (e) {
      debugPrint("Failed to connect target Bluetooth device: $e");
    }
  }

  void _startCleanupTimer() {
    _cleanupTimer?.cancel();
    _cleanupTimer = Timer.periodic(const Duration(seconds: 2), (timer) {
      if (_disposed) {
        timer.cancel();
        return;
      }
      final now = DateTime.now();
      final originalCount = discoveredDevices.length;

      discoveredDevices.removeWhere((d) {
        return now.difference(d.lastSeen) > _deviceTimeout;
      });

      if (discoveredDevices.length != originalCount) {
        _notifyDiscoveredDevices();
      }
    });
  }

  void _notifyDiscoveredDevices() {
    if (_disposed) return;
    try {
      blufDevicesMonitoring?.call(discoveredDevices);
    } catch (e) {
      debugPrint("Bluetooth device-list callback failed: $e");
    }
  }

  Future<void> stopScan() async {
    _cleanupTimer?.cancel();
    _cleanupTimer = null;

    final subscription = _scanSubscription;
    _scanSubscription = null;
    await _cancelSubscription(subscription, "Bluetooth scan");

    try {
      await FlutterBluePlus.stopScan().timeout(_scanOperationTimeout);
    } catch (e) {
      debugPrint("Failed to stop Bluetooth scan: $e");
    }
  }

  //MARK: - Connection related
  Future<void> connect(BluetoothDevice peripheral, {bool automatic = false}) {
    final completer = Completer<void>();
    final requestGeneration = ++_connectRequestGeneration;
    if (!automatic) {
      _reconnectTimer?.cancel();
      _reconnectTimer = null;
      _reconnectAttempt = 0;
      _lastPeripheral = peripheral;
    }

    Future<void> runConnection() async {
      if (requestGeneration != _connectRequestGeneration) {
        completer.completeError(
          StateError("Bluetooth connection request was superseded"),
        );
        return;
      }
      try {
        await _connectInternal(
          peripheral,
          automatic: automatic,
          requestGeneration: requestGeneration,
        );
        completer.complete();
      } catch (error, stackTrace) {
        completer.completeError(error, stackTrace);
      }
    }

    _connectTail = _connectTail.then<void>(
      (_) => runConnection(),
      onError: (Object _, StackTrace _) => runConnection(),
    );
    return completer.future;
  }

  Future<void> _connectInternal(
    BluetoothDevice peripheral, {
    required bool automatic,
    required int requestGeneration,
  }) async {
    if (_disposed) {
      throw StateError("Bluetooth manager has been disposed");
    }

    final disconnecting = _disconnectFuture;
    if (disconnecting != null) {
      await disconnecting;
      if (_disposed) {
        throw StateError("Bluetooth manager has been disposed");
      }
      if (requestGeneration != _connectRequestGeneration) {
        throw StateError("Bluetooth connection request was superseded");
      }
    }

    if (!automatic) {
      _reconnectTimer?.cancel();
      _reconnectTimer = null;
      _reconnectAttempt = 0;
    }

    final previousPeripheral = currentPeripheral;
    final generation = ++_connectionGeneration;
    final connectionMode = blueMode;
    _manualDisconnect = false;
    _lastPeripheral = peripheral;
    _connectingGeneration = generation;
    _connectedGeneration = null;
    _readyGeneration = null;
    _discoveryGeneration = null;
    _discoveryFuture = null;

    final previousStateSubscription = _connectionStateSubscription;
    _connectionStateSubscription = null;
    currentPeripheral = peripheral;
    _resetCharacteristics();

    try {
      await _cancelSubscription(
        previousStateSubscription,
        "Bluetooth connection state",
      );
      await _cancelCharacteristicSubscriptions();
      _throwIfConnectionIsStale(peripheral, generation);

      if (previousPeripheral != null &&
          previousPeripheral.remoteId != peripheral.remoteId &&
          !previousPeripheral.isDisconnected) {
        await _disconnectPeripheralQuietly(previousPeripheral);
        _throwIfConnectionIsStale(peripheral, generation);
      }

      final stateSubscription = peripheral.connectionState.listen(
        (state) => _handleConnectionState(peripheral, state, generation),
        onError: (Object error) {
          debugPrint("Bluetooth connection state stream failed: $error");
          if (isConnectionCurrent(peripheral, generation)) {
            _beginUnexpectedDisconnect(peripheral, generation);
          }
        },
      );
      if (!isConnectionCurrent(peripheral, generation)) {
        await _cancelSubscription(
          stateSubscription,
          "stale Bluetooth connection state",
        );
        _throwIfConnectionIsStale(peripheral, generation);
      }
      _connectionStateSubscription = stateSubscription;

      if (peripheral.isDisconnected) {
        await peripheral.connect(
          license: License.free,
          timeout: const Duration(seconds: 15),
          mtu: null,
          autoConnect: false,
        );
      }

      _throwIfConnectionIsStale(peripheral, generation);
      _connectedGeneration = generation;

      await _requestPreferredMtu(peripheral, generation);
      await _ensureServicesDiscovered(peripheral, generation, connectionMode);
      _throwIfConnectionIsStale(peripheral, generation);
      _validateRequiredCharacteristics(connectionMode);
      _reconnectTimer?.cancel();
      _reconnectTimer = null;
      _reconnectAttempt = 0;
      if (_readyGeneration != generation) {
        _readyGeneration = generation;
        _notifyConnectionState(peripheral, true);
      }
    } catch (error, stackTrace) {
      final ownsSession = isConnectionCurrent(peripheral, generation);
      if (ownsSession) {
        await _rollbackConnection(peripheral, generation);
      } else {
        final activePeripheral = currentPeripheral;
        if (activePeripheral == null ||
            activePeripheral.remoteId != peripheral.remoteId) {
          // A manual disconnect can invalidate this generation while the
          // platform connect call is still finishing. Do not leave that stale
          // peripheral physically connected and untracked.
          await _disconnectPeripheralQuietly(peripheral);
        }
      }
      if (automatic && ownsSession) {
        _scheduleReconnect(peripheral);
      }
      Error.throwWithStackTrace(error, stackTrace);
    } finally {
      if (_connectingGeneration == generation) {
        _connectingGeneration = null;
      }
    }
  }

  void _handleConnectionState(
    BluetoothDevice peripheral,
    BluetoothConnectionState state,
    int generation,
  ) {
    if (!isConnectionCurrent(peripheral, generation)) return;

    switch (state) {
      case BluetoothConnectionState.connected:
        _connectedGeneration = generation;
        break;
      case BluetoothConnectionState.disconnected:
        // Some platforms emit their initial disconnected state immediately
        // after listening. The connect Future owns that failure path.
        if (_connectingGeneration == generation &&
            _connectedGeneration != generation) {
          return;
        }
        _beginUnexpectedDisconnect(peripheral, generation);
        break;
    }
  }

  Future<void> _requestPreferredMtu(
    BluetoothDevice peripheral,
    int generation,
  ) async {
    if (!Platform.isAndroid) return;

    try {
      await peripheral.requestMtu(512, timeout: 8);
    } catch (e) {
      if (isConnectionCurrent(peripheral, generation)) {
        debugPrint("Failed to negotiate Bluetooth MTU: $e");
      }
    }
    _throwIfConnectionIsStale(peripheral, generation);
  }

  void _beginUnexpectedDisconnect(BluetoothDevice peripheral, int generation) {
    if (!isConnectionCurrent(peripheral, generation)) return;

    ++_connectionGeneration;
    final stateSubscription = _connectionStateSubscription;
    _connectionStateSubscription = null;
    currentPeripheral = null;
    _connectingGeneration = null;
    _connectedGeneration = null;
    _readyGeneration = null;
    _discoveryGeneration = null;
    _discoveryFuture = null;
    _resetCharacteristics();
    _notifyConnectionState(peripheral, false);
    _removeCachedPeripheral(peripheral);

    unawaited(
      _finishUnexpectedDisconnect(stateSubscription, peripheral, generation),
    );
  }

  Future<void> _finishUnexpectedDisconnect(
    StreamSubscription<BluetoothConnectionState>? stateSubscription,
    BluetoothDevice peripheral,
    int generation,
  ) async {
    await _cancelSubscription(stateSubscription, "Bluetooth connection state");
    await _cancelCharacteristicSubscriptions(generation: generation);
    if (autoReconnect) {
      _scheduleReconnect(peripheral);
    }
  }

  Future<void> _rollbackConnection(
    BluetoothDevice peripheral,
    int generation,
  ) async {
    if (!isConnectionCurrent(peripheral, generation)) return;

    final invalidatedGeneration = ++_connectionGeneration;
    final stateSubscription = _connectionStateSubscription;
    _connectionStateSubscription = null;
    currentPeripheral = null;
    _connectingGeneration = null;
    _connectedGeneration = null;
    _readyGeneration = null;
    _discoveryGeneration = null;
    _discoveryFuture = null;
    _resetCharacteristics();
    _removeCachedPeripheral(peripheral);

    await _cancelSubscription(stateSubscription, "Bluetooth connection state");
    await _cancelCharacteristicSubscriptions(generation: generation);
    if (_connectionGeneration == invalidatedGeneration &&
        currentPeripheral == null) {
      _notifyConnectionState(peripheral, false);
    }
    if (currentPeripheral?.remoteId != peripheral.remoteId) {
      await _disconnectPeripheralQuietly(peripheral);
    }
  }

  Future<void> _ensureServicesDiscovered(
    BluetoothDevice peripheral,
    int generation,
    int connectionMode,
  ) {
    if (_discoveryGeneration == generation && _discoveryFuture != null) {
      return _discoveryFuture!;
    }

    final future = _discoverServices(peripheral, generation, connectionMode);
    _discoveryGeneration = generation;
    _discoveryFuture = future;
    return future;
  }

  Future<void> _discoverServices(
    BluetoothDevice peripheral,
    int generation,
    int connectionMode,
  ) async {
    _throwIfConnectionIsStale(peripheral, generation);
    if (peripheral.isDisconnected) {
      throw StateError("Bluetooth device disconnected before discovery");
    }

    final services = await peripheral.discoverServices(timeout: 15);
    _throwIfConnectionIsStale(peripheral, generation);

    final supportedServices = services.where((service) {
      final uuid = service.uuid.toString().toLowerCase();
      return uuid == targetServiceUUID || uuid == danceTargetServiceUUID;
    }).toList();
    if (supportedServices.isEmpty) {
      throw StateError("StackChan Bluetooth service was not found");
    }

    for (final service in supportedServices) {
      await _discoverCharacteristics(
        peripheral,
        service,
        generation,
        connectionMode,
      );
      _throwIfConnectionIsStale(peripheral, generation);
    }
  }

  void _validateRequiredCharacteristics(int connectionMode) {
    if (connectionMode == 2) {
      if (writeMotionCharacteristic == null ||
          writeAvatarCharacteristic == null) {
        throw StateError("StackChan dance characteristics were not found");
      }
      return;
    }

    if (writeWifiSetCharacteristic == null) {
      throw StateError(
        "StackChan Wi-Fi configuration characteristic was not found",
      );
    }
  }

  Future<void> _discoverCharacteristics(
    BluetoothDevice peripheral,
    BluetoothService service,
    int generation,
    int connectionMode,
  ) async {
    for (final characteristic in service.characteristics) {
      _throwIfConnectionIsStale(peripheral, generation);
      _saveCharacteristicReference(characteristic);
      await _setupCharacteristicListener(
        peripheral,
        characteristic,
        generation,
        connectionMode,
      );
      _throwIfConnectionIsStale(peripheral, generation);

      final callback = characteristicCallback;
      if (callback != null) {
        await callback(peripheral, characteristic, generation);
        _throwIfConnectionIsStale(peripheral, generation);
      }
    }
  }

  Future<void> _setupCharacteristicListener(
    BluetoothDevice peripheral,
    BluetoothCharacteristic characteristic,
    int generation,
    int connectionMode,
  ) async {
    final uuid = characteristic.uuid.toString().toLowerCase();
    if (uuid != wifiSetCharacteristicUUID.toLowerCase()) {
      return;
    }
    if (connectionMode == 2) return;

    if (!characteristic.properties.notify &&
        !characteristic.properties.indicate) {
      throw StateError("Wi-Fi configuration characteristic cannot notify");
    }

    await characteristic.setNotifyValue(true);
    _throwIfConnectionIsStale(peripheral, generation);

    final subscription = characteristic.onValueReceived.listen(
      (value) {
        if (value.isEmpty || !isConnectionCurrent(peripheral, generation)) {
          return;
        }
        unawaited(_dispatchWifiNotification(value, peripheral, generation));
      },
      onError: (Object error) {
        if (isConnectionCurrent(peripheral, generation)) {
          debugPrint("Bluetooth notification stream failed: $error");
          _beginUnexpectedDisconnect(peripheral, generation);
        }
      },
    );
    _characteristicSubscriptions
        .putIfAbsent(generation, () => [])
        .add(subscription);
  }

  Future<void> _dispatchWifiNotification(
    List<int> value,
    BluetoothDevice peripheral,
    int generation,
  ) async {
    if (!isConnectionCurrent(peripheral, generation)) return;
    final callback = wifiSetCharacteristicCall;
    if (callback == null) return;

    try {
      await callback(value);
    } catch (e) {
      if (isConnectionCurrent(peripheral, generation)) {
        debugPrint("Bluetooth notification callback failed: $e");
      }
    }
  }

  void _saveCharacteristicReference(BluetoothCharacteristic characteristic) {
    final uuid = characteristic.uuid.toString().toLowerCase();
    switch (uuid) {
      case headCharacteristicUUID:
        writeHeadCharacteristic = characteristic;
        break;
      case wifiSetCharacteristicUUID:
        writeWifiSetCharacteristic = characteristic;
        break;
      case expressionCharacteristicUUID:
        writeExpressionCharacteristic = characteristic;
        break;
      case writeCharacteristicUUID:
        writeCharacteristic = characteristic;
        break;
      case motionCharacteristicUUID:
        writeMotionCharacteristic = characteristic;
        break;
      case avatarCharacteristicUUID:
        writeAvatarCharacteristic = characteristic;
        break;
      case rgbCharacteristicUUID:
        writeRGBCharacteristic = characteristic;
        break;
    }
  }

  Future<void> disconnectCurrentPeripheral() {
    ++_connectRequestGeneration;
    final inFlight = _disconnectFuture;
    if (inFlight != null) return inFlight;

    final operation = _disconnectCurrentPeripheralInternal();
    _disconnectFuture = operation;
    unawaited(
      operation.then<void>(
        (_) {
          if (identical(_disconnectFuture, operation)) {
            _disconnectFuture = null;
          }
        },
        onError: (Object error, StackTrace stackTrace) {
          if (identical(_disconnectFuture, operation)) {
            _disconnectFuture = null;
          }
        },
      ),
    );
    return operation;
  }

  Future<void> _disconnectCurrentPeripheralInternal() async {
    final peripheral = currentPeripheral;
    final generation = _connectionGeneration;
    _manualDisconnect = true;
    _lastPeripheral = null;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    final invalidatedGeneration = ++_connectionGeneration;

    final stateSubscription = _connectionStateSubscription;
    _connectionStateSubscription = null;
    currentPeripheral = null;
    _connectingGeneration = null;
    _connectedGeneration = null;
    _readyGeneration = null;
    _discoveryGeneration = null;
    _discoveryFuture = null;
    _resetCharacteristics();

    await _cancelSubscription(stateSubscription, "Bluetooth connection state");
    await _cancelCharacteristicSubscriptions(generation: generation);

    if (peripheral != null) {
      if (_connectionGeneration == invalidatedGeneration &&
          currentPeripheral == null) {
        _notifyConnectionState(peripheral, false);
      }
      if (currentPeripheral?.remoteId == peripheral.remoteId) return;
      await _disconnectPeripheralQuietly(peripheral);
    }
  }

  void _resetCharacteristics() {
    writeWifiSetCharacteristic = null;
    writeHeadCharacteristic = null;
    writeExpressionCharacteristic = null;
    writeCharacteristic = null;
    writeMotionCharacteristic = null;
    writeAvatarCharacteristic = null;
    writeRGBCharacteristic = null;
    _lastMotionPayload = null;
    _lastAvatarPayload = null;
    _lastRgbPayload = null;
    final pendingWrite = _pendingDanceWrite;
    _pendingDanceWrite = null;
    if (pendingWrite != null && !pendingWrite.completer.isCompleted) {
      pendingWrite.completer.complete(false);
    }
  }

  Future<void> _cancelCharacteristicSubscriptions({int? generation}) async {
    final subscriptions = <StreamSubscription<List<int>>>[];
    if (generation == null) {
      for (final generationSubscriptions
          in _characteristicSubscriptions.values) {
        subscriptions.addAll(generationSubscriptions);
      }
      _characteristicSubscriptions.clear();
    } else {
      subscriptions.addAll(
        _characteristicSubscriptions.remove(generation) ??
            <StreamSubscription<List<int>>>[],
      );
    }
    for (final subscription in subscriptions) {
      await _cancelSubscription(subscription, "Bluetooth notification");
    }
  }

  bool isConnectionCurrent(BluetoothDevice peripheral, int generation) {
    return !_disposed &&
        generation == _connectionGeneration &&
        currentPeripheral?.remoteId == peripheral.remoteId;
  }

  bool get hasReadyConnection {
    return !_disposed &&
        currentPeripheral != null &&
        _readyGeneration == _connectionGeneration;
  }

  void _throwIfConnectionIsStale(BluetoothDevice peripheral, int generation) {
    if (!isConnectionCurrent(peripheral, generation)) {
      throw StateError("Bluetooth connection was superseded");
    }
  }

  void _notifyConnectionState(BluetoothDevice peripheral, bool connected) {
    if (_disposed) return;
    try {
      connectionStateChanged?.call(peripheral, connected);
    } catch (e) {
      debugPrint("Bluetooth connection callback failed: $e");
    }
  }

  void _removeCachedPeripheral(BluetoothDevice peripheral) {
    for (final deviceInfo in discoveredDevices) {
      if (deviceInfo.device.remoteId == peripheral.remoteId) {
        final mac = _getDeviceId(deviceInfo);
        if (mac != null) {
          cachedDeviceMacs.remove(mac.toUpperCase());
        }
        return;
      }
    }
  }

  Future<void> _disconnectPeripheralQuietly(BluetoothDevice peripheral) async {
    if (peripheral.isDisconnected) return;
    try {
      await peripheral.disconnect(timeout: 10, queue: true, androidDelay: 2000);
    } catch (e) {
      debugPrint("Failed to disconnect Bluetooth device: $e");
    }
  }

  Future<void> _cancelSubscription(
    StreamSubscription? subscription,
    String label,
  ) async {
    if (subscription == null) return;
    try {
      await subscription.cancel().timeout(_subscriptionCancelTimeout);
    } catch (e) {
      debugPrint("Failed to cancel $label subscription: $e");
    }
  }

  //MARK: - Data sending
  Future<bool> sendHeadData(String data) {
    return _sendData(data, writeHeadCharacteristic, "Head data");
  }

  Future<bool> sendWifiSetData(
    String data, {
    BluetoothDevice? expectedDevice,
    int? connectionGeneration,
  }) {
    return _sendData(
      data,
      writeWifiSetCharacteristic,
      "WiFi set data",
      expectedDevice: expectedDevice,
      connectionGeneration: connectionGeneration,
    );
  }

  Future<bool> sendExpressionData(String data) {
    return _sendData(data, writeExpressionCharacteristic, "Expression data");
  }

  Future<bool> sendData(String data) {
    return _sendData(data, writeCharacteristic, "Data");
  }

  Future<bool> sendDanceData(DanceData data) {
    final peripheral = currentPeripheral;
    final generation = _connectionGeneration;
    if (peripheral == null ||
        !isConnectionCurrent(peripheral, generation) ||
        _readyGeneration != generation) {
      return Future<bool>.value(false);
    }

    final motionPayload = MotionData(
      pitchServo: data.pitchServo,
      yawServo: data.yawServo,
    ).toString();
    final avatarPayload = ExpressionData(
      leftEye: data.leftEye,
      rightEye: data.rightEye,
      mouth: data.mouth,
    ).toString();
    final rgbPayload = RgbData(
      leftRgbColor: data.leftRgbColor,
      leftRgbDuration: 0.3,
      rightRgbColor: data.rightRgbColor,
      rightRgbDuration: 0.3,
    ).toString();

    final write = _PendingDanceWrite(
      peripheral: peripheral,
      generation: generation,
      motionPayload: motionPayload,
      avatarPayload: avatarPayload,
      rgbPayload: rgbPayload,
    );

    if (_danceWriteActive) {
      // Real-time motion should converge on the newest state. Keeping only one
      // waiting frame prevents a slow or disconnected BLE link from building
      // an unbounded latency and memory backlog.
      final supersededWrite = _pendingDanceWrite;
      _pendingDanceWrite = write;
      if (supersededWrite != null && !supersededWrite.completer.isCompleted) {
        supersededWrite.completer.complete(false);
      }
    } else {
      _danceWriteActive = true;
      unawaited(_drainDanceWrites(write));
    }
    return write.completer.future;
  }

  Future<void> _drainDanceWrites(_PendingDanceWrite firstWrite) async {
    var write = firstWrite;
    while (true) {
      var sent = false;
      try {
        sent = await _sendDanceDataNow(
          write.peripheral,
          write.generation,
          write.motionPayload,
          write.avatarPayload,
          write.rgbPayload,
        );
      } catch (error) {
        _logDanceWriteError("dance frame", error);
      }
      if (!write.completer.isCompleted) {
        write.completer.complete(sent);
      }

      final nextWrite = _pendingDanceWrite;
      _pendingDanceWrite = null;
      if (nextWrite == null) {
        _danceWriteActive = false;
        return;
      }
      write = nextWrite;
    }
  }

  Future<bool> sendDanceDataWithinFrame(
    DanceData data,
    Duration minimumFrameDuration,
  ) async {
    final sendFuture = sendDanceData(data);
    if (minimumFrameDuration > Duration.zero) {
      await Future.delayed(minimumFrameDuration);
    }
    return sendFuture;
  }

  Future<bool> _sendDanceDataNow(
    BluetoothDevice peripheral,
    int generation,
    String motionPayload,
    String avatarPayload,
    String rgbPayload,
  ) async {
    if (!isConnectionCurrent(peripheral, generation)) return false;

    var hasSupportedCharacteristic = false;
    var allWritesSucceeded = true;

    final motionCharacteristic = writeMotionCharacteristic;
    if (motionCharacteristic != null) {
      hasSupportedCharacteristic = true;
      if (_lastMotionPayload != motionPayload) {
        final sent = await _writeDancePayload(
          peripheral,
          generation,
          motionCharacteristic,
          motionPayload,
          "motion",
        );
        allWritesSucceeded = allWritesSucceeded && sent;
        if (sent) _lastMotionPayload = motionPayload;
      }
    }

    final avatarCharacteristic = writeAvatarCharacteristic;
    if (avatarCharacteristic != null) {
      hasSupportedCharacteristic = true;
      if (_lastAvatarPayload != avatarPayload) {
        final sent = await _writeDancePayload(
          peripheral,
          generation,
          avatarCharacteristic,
          avatarPayload,
          "avatar",
        );
        allWritesSucceeded = allWritesSucceeded && sent;
        if (sent) _lastAvatarPayload = avatarPayload;
      }
    }

    final rgbCharacteristic = writeRGBCharacteristic;
    if (rgbCharacteristic != null) {
      hasSupportedCharacteristic = true;
      if (_lastRgbPayload != rgbPayload) {
        final sent = await _writeDancePayload(
          peripheral,
          generation,
          rgbCharacteristic,
          rgbPayload,
          "RGB",
        );
        allWritesSucceeded = allWritesSucceeded && sent;
        if (sent) _lastRgbPayload = rgbPayload;
      }
    }

    return hasSupportedCharacteristic && allWritesSucceeded;
  }

  Future<bool> _writeDancePayload(
    BluetoothDevice peripheral,
    int generation,
    BluetoothCharacteristic characteristic,
    String payload,
    String label,
  ) async {
    if (!isConnectionCurrent(peripheral, generation)) return false;

    final properties = characteristic.properties;
    if (!properties.write && !properties.writeWithoutResponse) {
      _logDanceWriteError(label, "characteristic is not writable");
      return false;
    }

    final withoutResponse =
        !properties.write && properties.writeWithoutResponse;
    try {
      await characteristic.write(
        utf8.encode(payload),
        withoutResponse: withoutResponse,
        allowLongWrite: !withoutResponse,
        timeout: _writeTimeoutSeconds,
      );
      return isConnectionCurrent(peripheral, generation);
    } catch (e) {
      _logDanceWriteError(label, e);
      return false;
    }
  }

  void _logDanceWriteError(String label, Object error) {
    final now = DateTime.now();
    if (_lastDanceWriteError == null ||
        now.difference(_lastDanceWriteError!) >= const Duration(seconds: 2)) {
      _lastDanceWriteError = now;
      debugPrint("Failed to send Bluetooth $label data: $error");
    }
  }

  Future<bool> _sendData(
    String data,
    BluetoothCharacteristic? characteristic,
    String type, {
    BluetoothDevice? expectedDevice,
    int? connectionGeneration,
  }) async {
    final peripheral = expectedDevice ?? currentPeripheral;
    final generation = connectionGeneration ?? _connectionGeneration;
    if (characteristic == null) {
      return false;
    }
    if (peripheral == null || !isConnectionCurrent(peripheral, generation)) {
      return false;
    }

    final dataToSend = utf8.encode(data);
    if (dataToSend.isEmpty) {
      return false;
    }

    try {
      final properties = characteristic.properties;
      if (!properties.write && !properties.writeWithoutResponse) {
        return false;
      }
      final withoutResponse =
          !properties.write && properties.writeWithoutResponse;
      await characteristic.write(
        dataToSend,
        withoutResponse: withoutResponse,
        allowLongWrite: !withoutResponse,
        timeout: _writeTimeoutSeconds,
      );

      return isConnectionCurrent(peripheral, generation);
    } catch (e) {
      debugPrint("Failed to send Bluetooth $type: $e");
      return false;
    }
  }

  Future<bool> reconnect() {
    final inFlight = _reconnectFuture;
    if (inFlight != null) return inFlight;

    final future = _reconnectInternal();
    _reconnectFuture = future;
    unawaited(
      future.then<void>(
        (_) {
          if (identical(_reconnectFuture, future)) {
            _reconnectFuture = null;
          }
        },
        onError: (Object error, StackTrace stackTrace) {
          if (identical(_reconnectFuture, future)) {
            _reconnectFuture = null;
          }
        },
      ),
    );
    return future;
  }

  Future<bool> _reconnectInternal() async {
    if (_disposed ||
        _manualDisconnect ||
        FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
      return false;
    }

    final peripheral = _lastPeripheral;
    if (peripheral == null) return false;
    if (currentPeripheral?.remoteId == peripheral.remoteId &&
        !peripheral.isDisconnected) {
      return _readyGeneration == _connectionGeneration;
    }

    try {
      await connect(peripheral, automatic: true);
      final generation = _connectionGeneration;
      if (!isConnectionCurrent(peripheral, generation)) return false;
      try {
        onReconnectSuccess?.call(peripheral, generation);
      } catch (e) {
        debugPrint("Bluetooth reconnect callback failed: $e");
      }
      return true;
    } catch (e) {
      debugPrint("Bluetooth reconnect failed: $e");
      return false;
    }
  }

  void _scheduleReconnect(BluetoothDevice peripheral) {
    if (_disposed ||
        _manualDisconnect ||
        FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on ||
        currentPeripheral != null ||
        (_reconnectTimer?.isActive ?? false)) {
      return;
    }

    _lastPeripheral = peripheral;
    final index = _reconnectAttempt
        .clamp(0, _reconnectDelays.length - 1)
        .toInt();
    final delay = _reconnectDelays[index];
    _reconnectAttempt++;
    _reconnectTimer = Timer(delay, () {
      _reconnectTimer = null;
      if (_disposed || _manualDisconnect) return;
      unawaited(reconnect());
    });
  }

  BlueReconnectCallback? onReconnectSuccess;

  Future<int?> readRssi(BluetoothDevice device) async {
    if (device.isDisconnected) return null;
    try {
      return await device.readRssi(timeout: 15);
    } catch (e) {
      return null;
    }
  }

  //MARK: - Resource release
  Future<void> dispose() {
    return _disposeFuture ??= _disposeInternal();
  }

  Future<void> _disposeInternal() async {
    _disposed = true;
    _manualDisconnect = true;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    _cleanupTimer?.cancel();
    _cleanupTimer = null;

    blufDevicesMonitoring = null;
    centralManagerDidUpdateState = null;
    characteristicCallback = null;
    connectionStateChanged = null;
    wifiSetCharacteristicCall = null;
    onReconnectSuccess = null;

    await disconnectCurrentPeripheral();
    await stopScan();

    final adapterSubscription = _adapterStateSubscription;
    _adapterStateSubscription = null;
    await _cancelSubscription(adapterSubscription, "Bluetooth adapter state");
    await _cancelCharacteristicSubscriptions();
  }
}

extension ScanResultListExtension on List<ScanResult> {
  void addOrUpdate(ScanResult result) {
    final index = indexWhere(
      (element) => element.device.remoteId == result.device.remoteId,
    );
    if (index == -1) {
      add(result);
    } else {
      this[index] = result;
    }
  }
}
