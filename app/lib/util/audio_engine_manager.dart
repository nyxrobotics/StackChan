/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:collection';
import 'dart:typed_data';

import 'package:flutter/cupertino.dart';
import 'package:opus_codec/opus_codec.dart' as opus_flutter;
import 'package:opus_codec_dart/opus_codec_dart.dart';
import 'package:permission_handler/permission_handler.dart';

import 'native_bridge.dart';

class _QueuedPcmFrame {
  const _QueuedPcmFrame({
    required this.engineGeneration,
    required this.playbackGeneration,
    required this.data,
  });

  final int engineGeneration;
  final int playbackGeneration;
  final ByteData data;
}

class AudioEngineManager {
  static final AudioEngineManager shared = AudioEngineManager._internal();

  AudioEngineManager._internal();

  late SimpleOpusEncoder simpleOpusEncoder;
  late SimpleOpusDecoder simpleOpusDecoder;

  bool _isInitialized = false;
  Future<void>? _initFuture;
  Future<void> _lifecycleTail = Future.value();
  bool _acceptAudioOperations = false;
  int _generation = 0;
  Function(Uint8List)? onAudioData;
  Function(double)? onDecibel;

  //🔥 Fixed Opus standard parameters (DO NOT CHANGE)
  static const int sampleRate = 16000;
  static const int channels = 1;
  static const int frameSamples = 320;
  static const int _maxPendingPcmFrames = 5;

  final Queue<_QueuedPcmFrame> _pcmPlaybackQueue = Queue<_QueuedPcmFrame>();
  Future<void>? _pcmDrainFuture;
  int _playbackGeneration = 0;
  bool _pcmDrainSuspended = false;

  Future<void> _enqueueLifecycle(Future<void> Function() operation) {
    final completer = Completer<void>();
    Future<void> runOperation() async {
      try {
        await operation();
        completer.complete();
      } catch (error, stackTrace) {
        completer.completeError(error, stackTrace);
      }
    }

    _lifecycleTail = _lifecycleTail.then<void>(
      (_) => runOperation(),
      onError: (Object _, StackTrace _) => runOperation(),
    );
    return completer.future;
  }

  Future<void> init() {
    if (_isInitialized && _acceptAudioOperations) {
      return Future.value();
    }
    final inFlight = _initFuture;
    if (inFlight != null) {
      return inFlight;
    }

    final generation = ++_generation;
    late final Future<void> future;
    future = _enqueueLifecycle(() async {
      if (generation == _generation && !_isInitialized) {
        await _initialize(generation);
      }
    });
    _initFuture = future;
    unawaited(
      future.then<void>(
        (_) {
          if (identical(_initFuture, future)) {
            _initFuture = null;
          }
        },
        onError: (Object _, StackTrace _) {
          if (identical(_initFuture, future)) {
            _initFuture = null;
          }
        },
      ),
    );
    return future;
  }

  Future<void> _initialize(int generation) async {
    SimpleOpusEncoder? encoder;
    SimpleOpusDecoder? decoder;
    try {
      WidgetsFlutterBinding.ensureInitialized();
      final opusLibrary = await opus_flutter.load();
      initOpus(opusLibrary);

      // StandardencodeController
      encoder = SimpleOpusEncoder(
        sampleRate: sampleRate,
        channels: channels,
        application: Application.voip,
      );
      decoder = SimpleOpusDecoder(sampleRate: sampleRate, channels: channels);
      if (generation != _generation) {
        _destroyEncoder(encoder);
        _destroyDecoder(decoder);
        return;
      }
      simpleOpusEncoder = encoder;
      simpleOpusDecoder = decoder;

      _pcmPlaybackQueue.clear();
      ++_playbackGeneration;
      _pcmDrainSuspended = false;
      _isInitialized = true;
      _acceptAudioOperations = true;
    } catch (error, stackTrace) {
      _destroyEncoder(encoder);
      _destroyDecoder(decoder);
      _isInitialized = false;
      _acceptAudioOperations = false;
      debugPrint("Audio engine initialization failed: $error\n$stackTrace");
      rethrow;
    }
  }

  void _destroyEncoder(SimpleOpusEncoder? encoder) {
    try {
      encoder?.destroy();
    } catch (_) {
      // Native cleanup is best-effort during failed initialization/shutdown.
    }
  }

  void _destroyDecoder(SimpleOpusDecoder? decoder) {
    try {
      decoder?.destroy();
    } catch (_) {
      // Native cleanup is best-effort during failed initialization/shutdown.
    }
  }

  //======================== play ========================
  Future<void> playOpus(Uint8List opusData) {
    if (!_isInitialized || !_acceptAudioOperations) {
      return Future<void>.value();
    }

    final engineGeneration = _generation;
    final playbackGeneration = _playbackGeneration;
    try {
      final pcm16 = simpleOpusDecoder.decode(input: opusData);
      final byteData = ByteData(pcm16.length * 2);
      for (int i = 0; i < pcm16.length; i++) {
        byteData.setInt16(i * 2, pcm16[i], Endian.little);
      }

      // Playback is real-time: when native playback cannot keep up, retain the
      // newest frames instead of accumulating latency behind stale audio.
      if (_pcmPlaybackQueue.length >= _maxPendingPcmFrames) {
        _pcmPlaybackQueue.removeFirst();
      }
      _pcmPlaybackQueue.addLast(
        _QueuedPcmFrame(
          engineGeneration: engineGeneration,
          playbackGeneration: playbackGeneration,
          data: byteData,
        ),
      );
      _startPcmDrain();
    } catch (error, stackTrace) {
      debugPrint("Failed to decode Opus audio: $error\n$stackTrace");
      return Future<void>.value();
    }

    return _pcmDrainFuture ?? Future<void>.value();
  }

  void _startPcmDrain() {
    if (_pcmDrainFuture != null ||
        _pcmDrainSuspended ||
        !_isInitialized ||
        !_acceptAudioOperations ||
        _pcmPlaybackQueue.isEmpty) {
      return;
    }

    final engineGeneration = _generation;
    late final Future<void> future;
    future = _drainPcmQueue(engineGeneration);
    _pcmDrainFuture = future;

    void finishDrain() {
      if (!identical(_pcmDrainFuture, future)) {
        return;
      }
      _pcmDrainFuture = null;
      if (_pcmPlaybackQueue.isNotEmpty) {
        _startPcmDrain();
      }
    }

    unawaited(
      future.then<void>(
        (_) => finishDrain(),
        onError: (Object error, StackTrace stackTrace) {
          debugPrint("PCM playback queue failed: $error\n$stackTrace");
          finishDrain();
        },
      ),
    );
  }

  Future<void> _drainPcmQueue(int engineGeneration) async {
    while (engineGeneration == _generation &&
        _isInitialized &&
        _acceptAudioOperations &&
        !_pcmDrainSuspended &&
        _pcmPlaybackQueue.isNotEmpty) {
      final frame = _pcmPlaybackQueue.removeFirst();
      if (frame.engineGeneration != _generation ||
          frame.playbackGeneration != _playbackGeneration) {
        continue;
      }

      try {
        await NativeBridge.shared.sendAudioStream(frame.data);
      } catch (error, stackTrace) {
        debugPrint("Failed to send PCM audio: $error\n$stackTrace");
      }
    }
  }

  Future<void> stopPlayOpus() async {
    final engineGeneration = _generation;
    final playbackGeneration = ++_playbackGeneration;
    _pcmDrainSuspended = true;
    _pcmPlaybackQueue.clear();

    final activeDrain = _pcmDrainFuture;
    if (activeDrain != null) {
      await activeDrain;
    }
    if (engineGeneration != _generation ||
        playbackGeneration != _playbackGeneration) {
      return;
    }

    try {
      await NativeBridge.shared.sendMessage(.stopPlayPCM);
    } catch (error, stackTrace) {
      debugPrint("Failed to stop PCM playback: $error\n$stackTrace");
    } finally {
      if (engineGeneration == _generation &&
          playbackGeneration == _playbackGeneration) {
        _pcmDrainSuspended = false;
        _startPcmDrain();
      }
    }
  }

  //====================== startRecord ======================
  Future<bool> startRecording() async {
    if (!_isInitialized || !_acceptAudioOperations) return false;
    final generation = _generation;

    try {
      //requestMicrophonepermission
      final perm = await Permission.microphone.request();
      if (!perm.isGranted) {
        return false;
      }

      if (generation != _generation ||
          !_isInitialized ||
          !_acceptAudioOperations) {
        return false;
      }

      var started = false;
      await _enqueueLifecycle(() async {
        if (generation != _generation ||
            !_isInitialized ||
            !_acceptAudioOperations) {
          return;
        }
        await NativeBridge.shared.sendMessage(.startRecording);
        started = true;
      });
      return started;
    } catch (error, stackTrace) {
      debugPrint("Failed to start audio recording: $error\n$stackTrace");
      return false;
    }
  }

  //====================== stopRecord ======================
  Future<void> stopRecording() async {
    await NativeBridge.shared.sendMessage(.stopRecording);
  }

  Future<void> dispose() {
    _acceptAudioOperations = false;
    ++_generation;
    ++_playbackGeneration;
    _pcmDrainSuspended = true;
    _pcmPlaybackQueue.clear();
    return _enqueueLifecycle(() async {
      if (_isInitialized) {
        _isInitialized = false;
        _destroyEncoder(simpleOpusEncoder);
        _destroyDecoder(simpleOpusDecoder);
      }
    });
  }
}
