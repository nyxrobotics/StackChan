/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:ffmpeg_kit_flutter_new/ffmpeg_kit.dart';
import 'package:ffmpeg_kit_flutter_new/return_code.dart';
import 'package:flutter/foundation.dart';
import 'package:just_audio/just_audio.dart';
import 'package:music_feature_analyzer/music_feature_analyzer.dart';
import 'package:path/path.dart' as path;
import 'package:path_provider/path_provider.dart';

class MusicInfo {
  int duration; //translated comment
  String filePath;
  String? title;
  String? artist;
  String? album;
  String? artwork;
  String? lyrics;

  MusicInfo(
    this.duration,
    this.filePath, {
    this.title,
    this.artist,
    this.album,
    this.artwork,
    this.lyrics,
  });

  ///loadmusicfileBytedata
  Future<Uint8List> loadData() async {
    try {
      final file = File(filePath);
      if (!await file.exists()) {
        throw FileSystemException("音乐文件不存在", file.path);
      }
      return await file.readAsBytes();
    } catch (e) {
      throw Exception("加载音乐文件失败: $e");
    }
  }

  String get mimeType {
    final ext = path.extension(filePath).toLowerCase();
    switch (ext) {
      case '.wav':
        return 'audio/wav';
      case '.m4a':
        return 'audio/mp4';
      case '.flac':
        return 'audio/flac';
      case '.mp3':
      default:
        return 'audio/mpeg';
    }
  }

  Future<List<double>> getProgressData({int targetSampleCount = 100}) async {
    if (targetSampleCount <= 0) {
      throw ArgumentError("目标采样点数必须大于0: $targetSampleCount");
    }
    final audioFile = File(filePath);
    if (!await audioFile.exists()) {
      throw FileSystemException("音频文件不存在", filePath);
    }

    final tempDir = await getTemporaryDirectory();
    final pcmFileName =
        "audio_waveform_${DateTime.now().microsecondsSinceEpoch}.pcm";
    final pcmFilePath = "${tempDir.path}/$pcmFileName";

    try {
      final command =
          '-loglevel error -hide_banner -i "$filePath" -f s16le -ac 1 -ar 16000 -vn "$pcmFilePath"';
      final session = await FFmpegKit.execute(command);
      final returnCode = await session.getReturnCode();
      if (!ReturnCode.isSuccess(returnCode)) {
        final error = await session.getAllLogsAsString();
        throw Exception("FFmpeg转换失败: 码=$returnCode, 错误=$error");
      }

      //Process PCM file, calculate chunk volume (RMS)
      final volumeData = await _processPcmFileForVolume(
        pcmFilePath,
        targetSampleCount,
      );
      return volumeData;
    } catch (e) {
      throw Exception("获取音频波动数据失败: $e");
    } finally {
      try {
        final pcmFile = File(pcmFilePath);
        if (await pcmFile.exists()) {
          await pcmFile.delete();
        }
      } catch (e) {
        //onlyPrintdeletefaillog,NotinterruptMainStreamProcess / Thread
      }
    }
  }

  ///Process PCM file, calculate chunk volume (RMS/decibel)
  Future<List<double>> _processPcmFileForVolume(
    String pcmPath,
    int targetSampleCount,
  ) async {
    final file = File(pcmPath);
    final bytes = await file.readAsBytes();
    const sampleSize = 2; //16-bit PCM = 2 bytes/sample
    final totalSamples = bytes.length ~/ sampleSize;

    //Boundary: return all zeros when no samples
    if (totalSamples == 0) {
      return List.filled(targetSampleCount, 0.0);
    }

    final byteData = ByteData.view(bytes.buffer);
    final volumeValues = <double>[];

    //calculateeachBlockShouldContainssampleCount / Number
    final samplesPerBlock = (totalSamples / targetSampleCount).ceil();

    //Calculate volume in chunks (RMS)
    for (int blockIndex = 0; blockIndex < targetSampleCount; blockIndex++) {
      //calculatecurrentBlocksamplerange
      final startSample = blockIndex * samplesPerBlock;
      final endSample = ((blockIndex + 1) * samplesPerBlock).clamp(
        0,
        totalSamples,
      );
      final blockSampleCount = endSample - startSample;

      //Boundary: chunk with no samples, volume is 0
      if (blockSampleCount <= 0) {
        volumeValues.add(0.0);
        continue;
      }

      //Calculate RMS of current chunk: reflects average volume in this period
      double sumOfSquares = 0.0;
      for (int i = startSample; i < endSample; i++) {
        //Read 16-bit little-endian PCM sample (range: -32768 ~ 32767)
        final int16Value = byteData.getInt16(i * sampleSize, Endian.little);
        //calculateSquareand
        sumOfSquares += (int16Value * int16Value).toDouble();
      }

      //RMS = sqrt(sum of squares / sample count)
      final rms = sqrt(sumOfSquares / blockSampleCount);
      //Normalize to 0~1 range (32767 is max value for 16-bit signed integer)
      final normalizedRms = (rms / 32767.0).clamp(0.0, 1.0);

      //Optional: Convert to decibels (dB) (closer to human perception, range: 0~1)
      //Decibel formula: 20 * log10(RMS / 32767), but handle 0 to avoid log(0)
      // final db = normalizedRms > 0 ? 20 * log10(normalizedRms) : -100;
      //final normalizedDb = (db + 100) / 100; // Map to 0~1
      // volumeValues.add(normalizedDb.clamp(0.0, 1.0));

      //Use normalized RMS directly (simpler, linear volume representation)
      volumeValues.add(normalizedRms);
    }

    return volumeValues;
  }
}

///Custom byte stream audio source (adapt just_audio)
class BytesAudioSource extends StreamAudioSource {
  final Uint8List bytes;
  final String contentType;
  final String? id;

  BytesAudioSource(this.bytes, {this.contentType = 'audio/mpeg', this.id});

  @override
  Future<StreamAudioResponse> request([int? start, int? end]) async {
    start ??= 0;
    end ??= bytes.length;
    return StreamAudioResponse(
      sourceLength: bytes.length,
      contentLength: end - start,
      offset: start,
      stream: Stream.value(bytes.sublist(start, end)),
      contentType: contentType,
    );
  }
}

class MusicUtil {
  //Singletonmode
  MusicUtil._internal() {
    _initAnalyzer();
    _setupPlayerListener(); //beforeinitlistener，avoid
  }

  static final MusicUtil shared = MusicUtil._internal();

  //Core player instance (just_audio)
  final AudioPlayer _audioPlayer = AudioPlayer();

  //playcompletecallback
  void Function()? _playbackCompletion;

  //musicduration(Second(s))
  double _musicDuration = 0.0;

  //currentplayprogress(Second(s))
  double _currentPosition = 0.0;

  //currentplaymusicinfo
  MusicInfo? _currentMusicInfo;
  int _operationGeneration = 0;
  int? _activePlaybackGeneration;
  int? _completionGeneration;
  int? _loopOverrideGeneration;
  LoopMode? _loopOverride;
  bool _isDisposed = false;

  int _beginPlaybackOperation() {
    final generation = ++_operationGeneration;
    _musicDuration = 0.0;
    _currentPosition = 0.0;
    _activePlaybackGeneration = null;
    _completionGeneration = null;
    _loopOverrideGeneration = null;
    _loopOverride = null;
    _playbackCompletion = null;
    _currentMusicInfo = null;
    return generation;
  }

  bool _isOperationCurrent(int generation) {
    return !_isDisposed && generation == _operationGeneration;
  }

  ///initmusicAnalyzer
  Future<void> _initAnalyzer() async {
    try {
      await MusicFeatureAnalyzer.initialize();
    } catch (error, stackTrace) {
      debugPrint(
        "Music feature analyzer initialization failed: "
        "$error\n$stackTrace",
      );
    }
  }

  ///configplayerlistener(System1Managerstate)
  void _setupPlayerListener() {
    _audioPlayer.setVolume(1.0).ignore();

    //playerstatelisten(Containsplaystateandhandlestate)
    _audioPlayer.playerStateStream.listen((PlayerState state) {
      //Playback completion check (handle completed status)
      if (state.processingState == ProcessingState.completed &&
          _audioPlayer.processingState == ProcessingState.completed) {
        _currentPosition = 0.0; //resetprogress

        // just_audio handles LoopMode.one itself. Completion belongs only to
        // the still-current non-looping source.
        final completion = _playbackCompletion;
        if (_audioPlayer.loopMode != LoopMode.one &&
            _activePlaybackGeneration == _operationGeneration) {
          final shouldCallCompletion =
              _completionGeneration == _operationGeneration;
          _activePlaybackGeneration = null;
          _playbackCompletion = null;
          _completionGeneration = null;
          _currentMusicInfo = null;
          _loopOverrideGeneration = null;
          _loopOverride = null;
          if (shouldCallCompletion) {
            try {
              completion?.call();
            } catch (_) {
              // A UI callback must not break the shared player state stream.
            }
          }
        }
      }

      //stopstateresetprogress
      if (state.processingState == ProcessingState.idle) {
        _currentPosition = 0.0;
      }
    });

    //durationChangelisten
    _audioPlayer.durationStream.listen((Duration? duration) {
      if (duration != null) {
        _musicDuration = duration.inMilliseconds / 1000.0;
      }
    });

    //playprogresslisten
    _audioPlayer.positionStream.listen((Duration position) {
      _currentPosition = position.inMilliseconds / 1000.0;
      //preventprogressExceeds totalduration
      if (_currentPosition > _musicDuration && _musicDuration > 0) {
        _currentPosition = _musicDuration;
      }
    });

    //errorlisten
    _audioPlayer.errorStream.listen((PlayerException? e) {
      if (e != null) {}
    });
  }

  ///playBytedataFormat / Formmusic
  Future<void> playMusicData(
    Uint8List data, {
    String contentType = 'audio/mpeg',
  }) async {
    if (_isDisposed) return;
    final generation = _beginPlaybackOperation();
    try {
      await _playMusicDataForOperation(
        generation,
        data,
        contentType: contentType,
      );
    } on PlayerException catch (e) {
      if (_isOperationCurrent(generation)) {
        _clearPlaybackState();
        throw Exception("播放失败: ${e.message}");
      }
    } catch (e) {
      if (_isOperationCurrent(generation)) {
        _clearPlaybackState();
        throw Exception("播放失败: $e");
      }
    }
  }

  ///playSinglemusic(playcompleteafterexecutecallback)
  Future<void> playMusicOnce(MusicInfo musicInfo, Function() completion) async {
    if (_isDisposed) return;
    final generation = _beginPlaybackOperation();
    await _playMusicForOperation(
      generation,
      musicInfo,
      isLoop: false,
      completion: completion,
    );
  }

  ///PlayOnlineMusic1Time(s),RepeatCallThenStopFrontFrom beginningPlay
  Future<void> playUrlMusicOnce(String? url, {Function()? completion}) async {
    if (url == null || _isDisposed) return;
    final generation = _beginPlaybackOperation();
    try {
      if (!await _resetPlayerForOperation(generation)) return;
      _playbackCompletion = completion;
      _completionGeneration = completion == null ? null : generation;

      final loopMode = _loopOverrideGeneration == generation
          ? _loopOverride!
          : LoopMode.off;
      await _audioPlayer.setLoopMode(loopMode);
      if (!_isOperationCurrent(generation)) return;
      await _audioPlayer.setUrl(url);
      if (!_isOperationCurrent(generation)) return;

      _activePlaybackGeneration = generation;
      await _audioPlayer.play();
    } on PlayerException catch (e) {
      if (_isOperationCurrent(generation)) {
        _clearPlaybackState();
        throw Exception("播放失败: ${e.message}");
      }
    } catch (e) {
      if (_isOperationCurrent(generation)) {
        _clearPlaybackState();
        throw Exception("播放失败: $e");
      }
    }
  }

  ///coreplaymethod（supportloop）
  Future<void> playMusic(MusicInfo? musicInfo, {bool isLoop = false}) async {
    if (musicInfo == null || _isDisposed) return;
    final generation = _beginPlaybackOperation();
    await _playMusicForOperation(generation, musicInfo, isLoop: isLoop);
  }

  Future<void> _playMusicForOperation(
    int generation,
    MusicInfo musicInfo, {
    required bool isLoop,
    Function()? completion,
  }) async {
    try {
      if (!await _resetPlayerForOperation(generation)) return;
      final data = await musicInfo.loadData();
      if (!_isOperationCurrent(generation)) return;
      final contentType = musicInfo.mimeType;
      await _playMusicDataForOperation(
        generation,
        data,
        contentType: contentType,
        musicInfo: musicInfo,
        isLoop: isLoop,
        completion: completion,
        resetPlayer: false,
      );
    } on PlayerException catch (e) {
      if (_isOperationCurrent(generation)) {
        _clearPlaybackState();
        throw Exception("播放失败: ${e.message}");
      }
    } catch (e) {
      if (_isOperationCurrent(generation)) {
        _clearPlaybackState();
        throw Exception("播放失败: $e");
      }
    }
  }

  Future<void> _playMusicDataForOperation(
    int generation,
    Uint8List data, {
    required String contentType,
    MusicInfo? musicInfo,
    bool isLoop = false,
    Function()? completion,
    bool resetPlayer = true,
  }) async {
    if (resetPlayer) {
      if (!await _resetPlayerForOperation(generation)) return;
    } else if (!_isOperationCurrent(generation)) {
      return;
    }

    _musicDuration = 0.0;
    _currentPosition = 0.0;
    _currentMusicInfo = musicInfo;
    _playbackCompletion = completion;
    _completionGeneration = completion == null ? null : generation;

    final loopMode = _loopOverrideGeneration == generation
        ? _loopOverride!
        : (isLoop ? LoopMode.one : LoopMode.off);
    await _audioPlayer.setLoopMode(loopMode);
    if (!_isOperationCurrent(generation)) return;

    final audioSource = BytesAudioSource(data, contentType: contentType);
    await _audioPlayer.setAudioSource(audioSource);
    if (!_isOperationCurrent(generation)) return;

    _activePlaybackGeneration = generation;
    await _audioPlayer.play();
  }

  Future<bool> _resetPlayerForOperation(int generation) async {
    await _audioPlayer.stop();
    if (!_isOperationCurrent(generation)) return false;
    await _audioPlayer.seek(Duration.zero);
    return _isOperationCurrent(generation);
  }

  void _clearPlaybackState() {
    _musicDuration = 0.0;
    _currentPosition = 0.0;
    _currentMusicInfo = null;
    _playbackCompletion = null;
    _completionGeneration = null;
    _activePlaybackGeneration = null;
    _loopOverrideGeneration = null;
    _loopOverride = null;
  }

  ///stopplay
  Future<void> stopMusic() async {
    if (_isDisposed) return;
    final generation = ++_operationGeneration;
    _clearPlaybackState();
    try {
      await _audioPlayer.stop();
    } catch (_) {
      return;
    }
    if (!_isOperationCurrent(generation)) return;
    try {
      await _audioPlayer.seek(Duration.zero);
    } catch (_) {
      return;
    }
    if (_isOperationCurrent(generation)) {
      _currentPosition = 0.0;
    }
  }

  ///pauseplay
  Future<void> pauseMusic() async {
    if (!_isDisposed && _audioPlayer.playing) {
      await _audioPlayer.pause();
    }
  }

  ///resumeplay
  Future<void> resumeMusic() async {
    if (!_isDisposed &&
        !_audioPlayer.playing &&
        _currentMusicInfo != null &&
        _activePlaybackGeneration == _operationGeneration) {
      await _audioPlayer.play();
    }
  }

  ///setloopplaystate
  void setMusicLoop(bool isLoop) {
    if (_isDisposed) return;
    final generation = _operationGeneration;
    final loopMode = isLoop ? LoopMode.one : LoopMode.off;
    _loopOverrideGeneration = generation;
    _loopOverride = loopMode;
    unawaited(_setLoopModeForOperation(generation, loopMode));
  }

  Future<void> _setLoopModeForOperation(
    int generation,
    LoopMode loopMode,
  ) async {
    if (!_isOperationCurrent(generation)) return;
    try {
      await _audioPlayer.setLoopMode(loopMode);
    } catch (_) {
      // A loop toggle must not fail the active playback operation.
    }
  }

  ///jumpplayprogress
  Future<void> seekTo(double seconds) async {
    if (_isDisposed || seconds < 0 || seconds > _musicDuration) return;
    final generation = _operationGeneration;
    await _audioPlayer.seek(Duration(seconds: seconds.toInt()));
    if (_isOperationCurrent(generation)) {
      _currentPosition = seconds;
    }
  }

  ///Set volume (0.0 ~ 1.0)
  Future<void> setVolume(double volume) async {
    if (_isDisposed || volume < 0.0 || volume > 1.0) return;
    await _audioPlayer.setVolume(volume);
  }

  ///setplayspeed
  Future<void> setPlaybackSpeed(double speed) async {
    if (_isDisposed || speed <= 0) return;
    await _audioPlayer.setSpeed(speed);
  }

  ///Getcurrentplayprogress(Second(s))
  double getCurrentPosition() => _currentPosition;

  ///GetmusicTotalduration(Second(s))
  double getMusicDuration() => _musicDuration;

  ///Getcurrentloopstate
  bool getIsLoop() => !_isDisposed && _audioPlayer.loopMode == LoopMode.one;

  ///GetcurrentplayerwhetherCurrentlyinplay
  bool isPlaying() => !_isDisposed && _audioPlayer.playing;

  ///releaseplayerAsset / ResourceSource(pagedisposewhenCall)
  Future<void> dispose() async {
    if (_isDisposed) return;
    _isDisposed = true;
    ++_operationGeneration;
    _clearPlaybackState();
    try {
      await _audioPlayer.stop();
    } catch (error, stackTrace) {
      debugPrint(
        "Failed to stop audio player during dispose: "
        "$error\n$stackTrace",
      );
    }
    try {
      await _audioPlayer.dispose();
    } catch (error, stackTrace) {
      debugPrint("Failed to dispose audio player: $error\n$stackTrace");
    }
  }

  ///improveaftermusicinfoparse(With / CarryVerboselog+cacheverify)
  Future<MusicInfo?> getMusicInfoAsync(String urlString) async {
    const tag = "MusicUtil/getMusicInfoAsync";
    try {
      //1. Parse URL
      final uri = Uri.parse(urlString);
      if (!uri.isAbsolute) {
        return null;
      }

      //2. Generate cache file info
      final extension = path.extension(uri.path);
      if (extension.isEmpty ||
          ![
            '.mp3',
            '.wav',
            '.m4a',
            '.flac',
          ].contains(extension.toLowerCase())) {
        return null;
      }
      final fileName = '${uri.hashCode.toRadixString(16)}$extension';
      //useDocumentDirectoryAnd / WhileNotisWhenwhenDirectory,avoidSystemautocleancachefile
      final cacheDir = await getApplicationDocumentsDirectory();
      final musicCacheDir = Directory(path.join(cacheDir.path, 'music_cache'));
      if (!await musicCacheDir.exists()) {
        await musicCacheDir.create(recursive: true);
      }
      final filePath = path.join(musicCacheDir.path, fileName);

      //3. Check cache file
      final file = File(filePath);
      if (await file.exists()) {
        final stat = await file.stat();
        final fileSizeKB = stat.size / 1024;
        if (fileSizeKB < 10) {
          await file.delete();
        } else {
          return await _extractMetadataFromFile(filePath, uri);
        }
      }

      //4. Download file
      await _downloadFile(uri, file);
      final stat = await file.stat();
      final fileSizeKB = stat.size / 1024;
      if (fileSizeKB < 10) {
        return null;
      }

      //5. Extract metadata
      return await _extractMetadataFromFile(filePath, uri);
    } catch (e, stackTrace) {
      debugPrint("$tag failed for $urlString: $e\n$stackTrace");
      return null;
    }
  }

  ///DownLoadfiletoLocalcache
  Future<void> _downloadFile(Uri uri, File file) async {
    final httpClient = HttpClient();
    try {
      final request = await httpClient.getUrl(uri);
      final response = await request.close();

      if (response.statusCode != HttpStatus.ok) {
        throw Exception("下载失败：状态码 ${response.statusCode}，URL=$uri");
      }

      await response.pipe(file.openWrite());
    } finally {
      httpClient.close();
    }
  }

  ///fromfileExtractmusicMetadata / Metadata
  Future<MusicInfo?> _extractMetadataFromFile(String filePath, Uri uri) async {
    const tag = "MusicUtil/_extractMetadataFromFile";
    try {
      final song = await MusicFeatureAnalyzer.metadata(filePath);
      if (song == null) {
        return null;
      }

      final durationSec = song.duration ~/ 1000; //convertas

      return MusicInfo(
        durationSec,
        filePath,
        title: song.title,
        artist: song.artist,
        album: song.album,
        artwork: song.albumArt,
      );
    } catch (e, stackTrace) {
      debugPrint("$tag failed for $filePath: $e\n$stackTrace");
      return null;
    }
  }

  ///cleanExpiredmusiccache(optional:Periodicallyclean)
  Future<void> clearExpiredCache({
    Duration maxAge = const Duration(days: 7),
  }) async {
    const tag = "MusicUtil/clearExpiredCache";
    try {
      final cacheDir = await getTemporaryDirectory();
      final files = await cacheDir.list().toList();
      final now = DateTime.now();
      int deletedCount = 0;

      for (final file in files) {
        if (file is File) {
          final extension = path.extension(file.path).toLowerCase();
          if (['.mp3', '.wav', '.m4a', '.flac'].contains(extension)) {
            final stat = await file.stat();
            final fileAge = now.difference(stat.modified);
            if (fileAge > maxAge) {
              await file.delete();
              deletedCount++;
            }
          }
        }
      }
      if (deletedCount > 0) {
        debugPrint("$tag deleted $deletedCount expired music files");
      }
    } catch (error, stackTrace) {
      debugPrint("$tag failed: $error\n$stackTrace");
    }
  }
}
