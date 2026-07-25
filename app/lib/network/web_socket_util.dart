/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

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

  bool _isConnected = false;
  bool _manualDisconnect = false;
  int _generation = 0;

  final Map<String, void Function()> _connectionObservers = {};
  final Map<String, void Function(dynamic)> _observers = {};

  String _urlString = '';

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
  Future<void> connect(String urlString) async {
    _manualDisconnect = false;
    _urlString = urlString;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;

    final generation = ++_generation;
    await _closeCurrentConnection();

    if (!_isConnectionRequestCurrent(generation, urlString) ||
        AppState.shared.deviceMac.isEmpty) {
      return;
    }

    late final WebSocket socket;
    try {
      final encryptedToken = RsaUtil.encrypt(
        getAuthorization(AppState.shared.deviceMac),
      );
      final headers = {ValueConstant.authorization: encryptedToken};
      socket = await WebSocket.connect(urlString, headers: headers);
    } catch (_) {
      if (_isConnectionRequestCurrent(generation, urlString)) {
        _isConnected = false;
        _scheduleReconnect(generation);
      }
      return;
    }

    if (!_isConnectionRequestCurrent(generation, urlString)) {
      await _closeSocket(null, socket);
      return;
    }

    _socket = socket;
    _isConnected = true;

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
    _reconnectTimer = Timer(const Duration(seconds: 1), () {
      _reconnectTimer = null;
      if (_manualDisconnect ||
          generation != _generation ||
          urlString != _urlString) {
        return;
      }
      unawaited(connect(urlString));
    });
  }

  /* =======================
   * Disconnect
   * ======================= */
  void disconnect() {
    _manualDisconnect = true;
    _urlString = '';
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
    try {
      await subscription?.cancel();
    } catch (_) {
      // The connection generation prevents stale callbacks from taking effect.
    }

    try {
      await socket?.close(WebSocketStatus.goingAway, 'Connection replaced');
    } catch (_) {
      // The socket is already unusable, so there is nothing else to clean up.
    }
  }

  bool _isConnectionRequestCurrent(int generation, String urlString) {
    return !_manualDisconnect &&
        generation == _generation &&
        urlString == _urlString;
  }

  bool _isCurrentSocket(WebSocket socket, int generation) {
    return _isConnectionRequestCurrent(generation, _urlString) &&
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
    for (final observer in _observers.values) {
      observer(message);
    }
  }

  bool get isConnected => _isConnected && _socket?.readyState == WebSocket.open;
}
