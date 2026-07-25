/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:stack_chan/app_state.dart';
import 'package:stack_chan/model/msg_type.dart';

void main() {
  group('AppState WebSocket frame parsing', () {
    final appState = AppState();

    test('decodes a complete frame', () {
      final result = appState.parseMessage(
        Uint8List.fromList([
          MsgType.deviceOnline.value,
          0,
          0,
          0,
          3,
          0x41,
          0x42,
          0x43,
        ]),
      );

      expect(result.$1, MsgType.deviceOnline);
      expect(result.$2, Uint8List.fromList([0x41, 0x42, 0x43]));
    });

    test('rejects unknown, incomplete, and truncated frames', () {
      expect(appState.parseMessage(Uint8List(0)), (null, null));
      expect(appState.parseMessage(Uint8List.fromList([0xFF, 0, 0, 0, 0])), (
        null,
        null,
      ));
      expect(
        appState.parseMessage(
          Uint8List.fromList([MsgType.opus.value, 0, 0, 0, 2, 0x01]),
        ),
        (null, null),
      );
    });
  });
}
