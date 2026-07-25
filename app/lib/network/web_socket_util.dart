/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';

import '../app_state.dart';
import '../model/msg_type.dart';
import '../util/rsa_util.dart';
import '../util/value_constant.dart';

class WebSocketUtil {
  WebSocketUtil._internal();

  static final WebSocketUtil shared = WebSocketUtil._internal();

  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;
  Timer? _reconnectTimer;
  Future<void>? _connectFuture;
  String? _connectFutureUrl;
  String? _connectFutureMac;

  bool _isConnected = false;
  bool _manualDisconnect = false;
  int _generation = 0;
  int _reconnectAttempt = 0;

  static const Duration _connectTimeout = Duration(seconds: 15);
  static const Duration _closeTimeout = Duration(seconds: 5);
  static const int _maxReconnectDelaySeconds = 30;

  final Map<String, void Function()> _connectionObservers = {};
  final Map<String, void Function(dynamic)> _observers = {};

  String _urlString = '';
  String _deviceMac = '';

  /* =======================
   * Authorization
   * ======================= */
  String getAuthorization(String mac) {
    final rand = Random();
    final randomPart = List.generate(
      mac.length,
      (_) =>
          ValueConstant.characters[rand.nextInt(
            ValueConstant.characters.length,
          )],
    ).join();

    final timestamp = DateTime.now().millisecondsSinceEpoch ~/ 1000;
    return '$mac|$randomPart|$timestamp';
  }

  /* =======================
   * Connect
   * ======================= */
  Future<void> connect(String urlString) {
    return _connectInternalTracked(urlString, AppState.shared.deviceMac);
  }

  Future<void> _connectInternalTracked(String urlString, String deviceMac) {
    final inFlight = _connectFuture;
    if (inFlight != null &&
        !_manualDisconnect &&
        _connectFutureUrl == urlString &&
        _connectFutureMac == deviceMac) {
      return inFlight;
    }

    late final Future<void> future;
    future = (() async {
      try {
        await _connectInternal(urlString, deviceMac);
      } finally {
        if (identical(_connectFuture, future)) {
          _connectFuture = null;
          _connectFutureUrl = null;
          _connectFutureMac = null;
        }
      }
    })();
    _connectFuture = future;
    _connectFutureUrl = urlString;
    _connectFutureMac = deviceMac;
    return future;
  }

  Future<void> _connectInternal(String urlString, String deviceMac) async {
    _manualDisconnect = false;
    _urlString = urlString;
    _deviceMac = deviceMac;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;

    final generation = ++_generation;
    await _closeCurrentConnection();

    if (!_isConnectionRequestCurrent(generation, urlString, deviceMac) ||
        deviceMac.isEmpty) {
      return;
    }

    late final WebSocket socket;
    try {
      final encryptedToken = RsaUtil.encrypt(getAuthorization(deviceMac));
      final headers = {ValueConstant.authorization: encryptedToken};
      final pendingSocket = WebSocket.connect(urlString, headers: headers);
      try {
        socket = await pendingSocket.timeout(_connectTimeout);
      } on TimeoutException {
        // Dart cannot cancel WebSocket.connect. Close a socket that arrives
        // after our timeout so it cannot leak in the background.
        unawaited(
          pendingSocket.then<void>(
            (lateSocket) => _closeSocket(null, lateSocket),
            onError: (Object error, StackTrace stackTrace) {
              debugPrint(
                "Timed-out WebSocket connection failed while closing: "
                "$error\n$stackTrace",
              );
            },
          ),
        );
        rethrow;
      }
    } catch (_) {
      if (_isConnectionRequestCurrent(generation, urlString, deviceMac)) {
        _isConnected = false;
        _scheduleReconnect(generation);
      }
      return;
    }

    if (!_isConnectionRequestCurrent(generation, urlString, deviceMac)) {
      await _closeSocket(null, socket);
      return;
    }

    _socket = socket;
    _isConnected = true;
    _reconnectAttempt = 0;

    try {
      _subscription = socket.listen(
        (message) {
          if (_isCurrentSocket(socket, generation)) {
            _handleMessage(message);
          }
        },
        onError: (Object _) => _handleError(socket, generation),
        onDone: () => _handleDone(socket, generation),
        cancelOnError: true,
      );
    } catch (_) {
      _handleConnectionLost(socket, generation);
      return;
    }

    if (_isCurrentSocket(socket, generation)) {
      _notifyConnectionObservers();
    }
  }

  /* =======================
   * Message Handling
   * ======================= */
  void _handleMessage(dynamic message) {
    final isPing = replyPong(message);
    if (!isPing) {
      _notifyObservers(message);
    }
  }

  void _handleError(WebSocket socket, int generation) {
    _handleConnectionLost(socket, generation);
  }

  void _handleDone(WebSocket socket, int generation) {
    _handleConnectionLost(socket, generation);
  }

  void _handleConnectionLost(WebSocket socket, int generation) {
    if (!_isCurrentSocket(socket, generation)) {
      return;
    }

    final subscription = _subscription;
    _subscription = null;
    _socket = null;
    _isConnected = false;

    unawaited(_closeSocket(subscription, socket));
    _scheduleReconnect(generation);
  }

  bool replyPong(dynamic message) {
    if (message is Uint8List) {
      final result = AppState.shared.parseMessage(message);
      final msgType = result.$1;

      if (msgType != null) {
        switch (msgType) {
          case MsgType.ping:
            AppState.shared.sendWebSocketMessage(.pong);
            return true;
          default:
            return false;
        }
      }
    }
    return false;
  }

  /* =======================
   * Send
   * ======================= */
  void sendString(String message) {
    final socket = _socket;
    final generation = _generation;
    if (socket == null) {
      return;
    }

    try {
      socket.add(message);
    } catch (_) {
      _handleConnectionLost(socket, generation);
    }
  }

  void send(Uint8List data) {
    final socket = _socket;
    final generation = _generation;
    if (socket == null) {
      return;
    }

    try {
      socket.add(data);
    } catch (_) {
      _handleConnectionLost(socket, generation);
    }
  }

  /* =======================
   * Reconnect
   * ======================= */
  void _scheduleReconnect(int generation) {
    if (_manualDisconnect ||
        generation != _generation ||
        _urlString.isEmpty ||
        (_reconnectTimer?.isActive ?? false)) {
      return;
    }

    final urlString = _urlString;
    final deviceMac = _deviceMac;
    final exponent = min(_reconnectAttempt, 5);
    final baseDelayMs = min(
      _maxReconnectDelaySeconds * 1000,
      1000 * (1 << exponent),
    );
    _reconnectAttempt++;
    final jitterMs = (baseDelayMs * (Random().nextDouble() * 0.4 - 0.2))
        .round();
    _reconnectTimer = Timer(Duration(milliseconds: baseDelayMs + jitterMs), () {
      _reconnectTimer = null;
      if (_manualDisconnect ||
          generation != _generation ||
          urlString != _urlString ||
          deviceMac != _deviceMac ||
          deviceMac.isEmpty ||
          deviceMac != AppState.shared.deviceMac) {
        return;
      }
      unawaited(_connectInternalTracked(urlString, deviceMac));
    });
  }

  /* =======================
   * Disconnect
   * ======================= */
  void disconnect() {
    _manualDisconnect = true;
    _urlString = '';
    _deviceMac = '';
    _reconnectAttempt = 0;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    ++_generation;

    final subscription = _subscription;
    final socket = _socket;
    _subscription = null;
    _socket = null;
    _isConnected = false;

    unawaited(_closeSocket(subscription, socket));
  }

  Future<void> _closeCurrentConnection() async {
    final subscription = _subscription;
    final socket = _socket;
    _subscription = null;
    _socket = null;
    _isConnected = false;

    await _closeSocket(subscription, socket);
  }

  Future<void> _closeSocket(
    StreamSubscription<dynamic>? subscription,
    WebSocket? socket,
  ) async {
    Future<void> cancelSubscription() async {
      if (subscription == null) {
        return;
      }
      try {
        await subscription.cancel().timeout(_closeTimeout);
      } catch (_) {
        // The connection generation prevents stale callbacks from taking effect.
      }
    }

    Future<void> closeSocket() async {
      if (socket == null) {
        return;
      }
      try {
        await socket
            .close(WebSocketStatus.goingAway, 'Connection replaced')
            .timeout(_closeTimeout);
      } catch (_) {
        // The socket is already unusable, so there is nothing else to clean up.
      }
    }

    await Future.wait([cancelSubscription(), closeSocket()]);
  }

  bool _isConnectionRequestCurrent(
    int generation,
    String urlString,
    String deviceMac,
  ) {
    return !_manualDisconnect &&
        generation == _generation &&
        urlString == _urlString &&
        deviceMac == _deviceMac &&
        deviceMac.isNotEmpty &&
        deviceMac == AppState.shared.deviceMac;
  }

  bool _isCurrentSocket(WebSocket socket, int generation) {
    return _isConnectionRequestCurrent(generation, _urlString, _deviceMac) &&
        identical(socket, _socket);
  }

  /* =======================
   * Observer
   * ======================= */
  void addConnectionObserver(String key, void Function() observer) {
    _connectionObservers[key] = observer;
  }

  void removeConnectionObserver(String key) {
    _connectionObservers.remove(key);
  }

  void _notifyConnectionObservers() {
    final observers = List<void Function()>.of(_connectionObservers.values);
    for (final observer in observers) {
      try {
        observer();
      } catch (_) {
        // One observer must not prevent the remaining observers from running.
      }
    }
  }

  void addObserver(String key, void Function(dynamic message) observer) {
    _observers[key] = observer;
  }

  void removeObserver(String key) {
    _observers.remove(key);
  }

  void removeAllObservers() {
    _observers.clear();
  }

  void _notifyObservers(dynamic message) {
    final observers = List<void Function(dynamic)>.of(_observers.values);
    for (final observer in observers) {
      try {
        observer(message);
      } catch (_) {
        // One observer must not prevent the remaining observers from running.
      }
    }
  }

  bool get isConnected => _isConnected && _socket?.readyState == WebSocket.open;
}
