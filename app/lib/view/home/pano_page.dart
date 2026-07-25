/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/cupertino.dart';
import 'package:get/get.dart';
import 'package:opencv_dart/opencv.dart' as cv;
import 'package:stack_chan/app_state.dart';

import '../../model/expression_data.dart';
import '../../network/web_socket_util.dart';
import '../../util/extension.dart';

class PanoPage extends StatefulWidget {
  const PanoPage({super.key});

  @override
  State<StatefulWidget> createState() => _PanoPageState();
}

class _PanoPageState extends State<PanoPage> {
  final String tag = "PanoPage";

  bool recordSwitch = false;
  bool _disposed = false;
  int _captureGeneration = 0;
  int _assemblyGeneration = 0;
  String? _captureDeviceMac;

  RxList<Uint8List> imageDataList = RxList([]);

  Rxn<Uint8List> panoImage = Rxn();

  RxBool isTakingPhotos = false.obs;
  RxBool isLoading = false.obs;
  final Duration motionDelay = Duration(milliseconds: 500);
  final Duration captureDelay = Duration(milliseconds: 500);

  final SliverGridDelegateWithFixedCrossAxisCount gridDelegate =
      SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: 3,
        crossAxisSpacing: 8,
        mainAxisSpacing: 8,
        childAspectRatio: 1,
      );

  //data
  List<MotionData> motionList = [
    //1
    MotionData(
      pitchServo: MotionDataItem(angle: 0, speed: 0),
      yawServo: MotionDataItem(angle: 900, speed: 0),
    ),

    //2 * 7
    MotionData(
      pitchServo: MotionDataItem(angle: 1280, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 853, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 426, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 0, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -426, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -853, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -1280, speed: 0),
      yawServo: MotionDataItem(angle: 675, speed: 0),
    ),

    //3 * 7
    MotionData(
      pitchServo: MotionDataItem(angle: -1280, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -853, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -426, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 0, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 426, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 853, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 1280, speed: 0),
      yawServo: MotionDataItem(angle: 450, speed: 0),
    ),

    //4 * 7
    MotionData(
      pitchServo: MotionDataItem(angle: 1280, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 853, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 426, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 0, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -426, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -853, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -1280, speed: 0),
      yawServo: MotionDataItem(angle: 225, speed: 0),
    ),

    //5 * 7
    MotionData(
      pitchServo: MotionDataItem(angle: -1280, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -853, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: -426, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 0, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 426, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 853, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
    MotionData(
      pitchServo: MotionDataItem(angle: 1280, speed: 0),
      yawServo: MotionDataItem(angle: 0, speed: 0),
    ),
  ];

  @override
  void initState() {
    super.initState();
    WebSocketUtil.shared.addObserver(tag, (message) {
      if (_disposed) return;
      if (message is Uint8List) {
        final result = AppState.shared.parseMessage(message);
        final msgType = result.$1;
        final parsedData = result.$2;
        if (msgType != null) {
          switch (msgType) {
            case .jpeg:
              if (parsedData != null) {
                if (recordSwitch) {
                  imageDataList.add(parsedData);
                  recordSwitch = false;
                }
              }
              break;
            default:
              break;
          }
        }
      }
    });
  }

  @override
  void dispose() {
    _disposed = true;
    ++_captureGeneration;
    ++_assemblyGeneration;
    recordSwitch = false;
    WebSocketUtil.shared.removeObserver(tag);
    final captureDeviceMac = _captureDeviceMac;
    _captureDeviceMac = null;
    if (captureDeviceMac != null) {
      AppState.shared.sendWebSocketMessage(
        .offCamera,
        data: captureDeviceMac.toUint8List(),
      );
    }
    super.dispose();
  }

  Future<void> startTakingPhotos() async {
    final deviceMac = AppState.shared.deviceMac;
    if (deviceMac.isEmpty) {
      AppState.shared.showToast("Please re-attempt after binding the device.");
      return;
    }

    if (_disposed || isTakingPhotos.value) return;

    final generation = ++_captureGeneration;
    var cameraOpened = false;
    try {
      isTakingPhotos.value = true;
      imageDataList.clear();
      panoImage.value = null;
      AppState.shared.showToast("Start panoramic shooting...");

      AppState.shared.sendWebSocketMessage(
        .onCamera,
        data: deviceMac.toUint8List(),
      );
      cameraOpened = true;
      _captureDeviceMac = deviceMac;
      await Future.delayed(Duration(milliseconds: 300));
      if (!_isCaptureCurrent(generation, deviceMac)) return;

      for (final motion in motionList) {
        final jsonString = deviceMac + motion.toString();
        AppState.shared.sendWebSocketMessage(
          .controlMotion,
          data: jsonString.toUint8List(),
        );

        await Future.delayed(motionDelay);
        if (!_isCaptureCurrent(generation, deviceMac)) return;

        recordSwitch = true;

        await Future.delayed(captureDelay);
        if (!_isCaptureCurrent(generation, deviceMac)) return;
      }

      AppState.shared.showToast("The shooting is complete.");
      unawaited(startAssemble());
    } catch (e) {
      if (_isCaptureCurrent(generation, deviceMac)) {
        AppState.shared.showToast(
          "The shooting was unsuccessful.：${e.toString()}",
        );
      }
    } finally {
      if (cameraOpened && _captureDeviceMac == deviceMac) {
        _captureDeviceMac = null;
        AppState.shared.sendWebSocketMessage(
          .offCamera,
          data: deviceMac.toUint8List(),
        );
      }
      if (!_disposed && generation == _captureGeneration) {
        isTakingPhotos.value = false;
      }
      recordSwitch = false;
    }
  }

  bool _isCaptureCurrent(int generation, String deviceMac) {
    return !_disposed &&
        generation == _captureGeneration &&
        AppState.shared.deviceMac == deviceMac;
  }

  Future<void> startAssemble() async {
    if (imageDataList.length < 5) {
      AppState.shared.showToast(
        "At least 5 photos are needed to stitch together a panoramic image!",
      );
      return;
    }
    if (_disposed || isLoading.value) return;
    final generation = ++_assemblyGeneration;
    isLoading.value = true;
    panoImage.value = null;

    List<cv.Mat> mats = [];
    cv.VecMat? vecMat;
    cv.Stitcher? stitcher;
    cv.Mat? resultMat;

    try {
      for (final data in imageDataList) {
        final mat = await cv.imdecodeAsync(data, cv.IMREAD_COLOR);
        if (_disposed || generation != _assemblyGeneration) {
          mat.dispose();
          return;
        }
        if (mat.isEmpty) {
          mat.dispose();
          throw Exception("Invalid image data");
        }
        mats.add(mat);
      }
      vecMat = cv.VecMat.fromList(mats);
      stitcher = cv.Stitcher.create(mode: .PANORAMA);
      final (status, result) = await stitcher.stitchAsync(vecMat);
      resultMat = result;
      if (_disposed || generation != _assemblyGeneration) return;
      if (status != cv.StitcherStatus.OK) {
        throw Exception("Stitch error code: $status");
      }
      final (resultStatus, jpeg) = await cv.imencodeAsync(".jpg", result);
      if (_disposed || generation != _assemblyGeneration) return;
      if (!resultStatus) {
        throw Exception("Encode failed");
      }
      panoImage.value = jpeg;
      AppState.shared.showToast("Stitch success!");
    } catch (e) {
      if (!_disposed && generation == _assemblyGeneration) {
        AppState.shared.showToast("Error: ${e.toString()}");
      }
    } finally {
      resultMat?.dispose();
      for (var mat in mats) {
        mat.dispose();
      }
      vecMat?.dispose();
      stitcher?.dispose();
      if (!_disposed && generation == _assemblyGeneration) {
        isLoading.value = false;
      }
    }
  }

  Widget buildImageItem(BuildContext context, int index) {
    final data = imageDataList[index];
    return Stack(
      fit: .expand,
      children: [
        ClipRRect(
          borderRadius: .circular(8),
          child: Image.memory(data, fit: .cover),
        ),
        Positioned(
          top: 4,
          right: 4,
          child: CupertinoButton(
            padding: .zero,
            minimumSize: Size(26, 26),
            child: Icon(
              CupertinoIcons.clear_circled_solid,
              color: CupertinoColors.systemRed,
              size: 26,
            ),
            onPressed: () {
              imageDataList.removeAt(index);
            },
          ),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemBackground.resolveFrom(context),
      navigationBar: CupertinoNavigationBar.large(
        largeTitle: Text("Panorama"),
        backgroundColor: CupertinoColors.systemBackground.resolveFrom(context),
      ),
      child: Padding(
        padding: .only(
          top: 15,
          bottom: 15 + MediaQuery.paddingOf(context).bottom,
          left: 15,
          right: 15,
        ),
        child: Column(
          children: [
            Obx(() {
              if (panoImage.value != null) {
                return Column(
                  mainAxisSize: .min,
                  spacing: 8,
                  children: [
                    Text("Panorama Result"),
                    Container(
                      height: 220,
                      decoration: BoxDecoration(
                        borderRadius: .circular(10),
                        image: DecorationImage(
                          image: MemoryImage(panoImage.value!),
                          fit: .contain,
                        ),
                      ),
                    ),
                  ],
                );
              }
              return SizedBox.shrink();
            }),
            Expanded(
              child: Obx(() {
                if (imageDataList.isEmpty) {
                  return const Center(child: Text("No photos, take first"));
                }
                return GridView.builder(
                  itemCount: imageDataList.length,
                  gridDelegate: gridDelegate,
                  itemBuilder: buildImageItem,
                );
              }),
            ),
            CupertinoButton.filled(
              child: const SizedBox(
                width: double.infinity,
                child: Center(child: Text("Generate")),
              ),
              onPressed: () {
                startTakingPhotos();
              },
            ),
          ],
        ),
      ),
    );
  }
}
