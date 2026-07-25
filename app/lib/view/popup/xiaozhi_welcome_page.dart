/*
SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
SPDX-License-Identifier: MIT
*/

import 'dart:async';

import 'package:flutter/cupertino.dart';
import 'package:get/get.dart';
import 'package:pull_down_button/pull_down_button.dart';
import 'package:stack_chan/model/XiaoZhi/agent.dart';
import 'package:stack_chan/util/extension.dart';
import 'package:stack_chan/util/music_util.dart';
import 'package:stack_chan/view/popup/device_wifi_config.dart';

import '../../app_state.dart';
import '../../model/XiaoZhi/XiaoZhi_model.dart';
import '../../model/XiaoZhi/agent_create.dart';
import '../../model/XiaoZhi/common_mcp_tool.dart';
import '../../model/XiaoZhi/tts_list.dart';
import '../../util/XiaoZhi_util.dart';
import '../../util/mac_address_validator.dart';
import '../../util/value_constant.dart';

class XiaoZhiWelcomePage extends StatefulWidget {
  const XiaoZhiWelcomePage({super.key, this.isWelCome});

  final bool? isWelCome;

  @override
  State<StatefulWidget> createState() => _XiaoZhiWelcomePageState();
}

class XiaoZhiEditAgentModel extends GetxController {
  Agent? agent;

  final RxBool isLoading = false.obs;

  final TextEditingController agentNameController = TextEditingController();
  final TextEditingController assistantNameController = TextEditingController();
  final TextEditingController characterController = TextEditingController();
  final TextEditingController memoryController = TextEditingController();
  Worker? _languageWorker;
  bool _disposed = false;
  int _lifecycleGeneration = 0;

  final Rxn<ModelData> selectedModel = Rxn();
  final Rxn<TTsVoice> selectedTtsVoice = Rxn();
  final RxString selectedLanguage = "".obs;
  final RxString ttsSpeed = "normal".obs;
  final RxInt ttsPitch = 0.obs;
  final RxString asrSpeed = "normal".obs;
  final RxString memoryType = "SHORT_TERM".obs;
  final List<String> selectedMcpEndpoints = [];

  TTsList? ttsData;
  RxList<TTsVoice> ttsList = RxList([]);

  RxList<String> languageList = RxList([]);

  RxList<ModelData> modelList = RxList([]);
  RxList<CommonMcpTool> commonMcpTools = RxList([]);

  final List<String> speedList = ["slow", "normal", "fast"];
  final List<int> pitchList = [-2, -1, 0, 1, 2];
  final List<String> memoryTypeList = ["OFF", "SHORT_TERM"];

  bool _isCurrent(int generation) {
    return !_disposed && generation == _lifecycleGeneration;
  }

  Future<void> initPageData(bool isWelCome) async {
    if (_disposed) return;
    final generation = ++_lifecycleGeneration;
    isLoading.value = false;
    agent = null;
    selectedMcpEndpoints.clear();
    _languageWorker?.dispose();
    _languageWorker = ever(selectedLanguage, (lang) {
      if (_isCurrent(generation)) {
        _updateTtsVoiceList(lang);
      }
    });

    try {
      await loadCommonMcpTools(generation);
      await loadTtsList(generation);
      await loadModelList(generation);
      if (!_isCurrent(generation)) return;

      final deviceMac = AppState.shared.deviceMac;
      await _loadAgentForDevice(isWelCome, generation, deviceMac);
      if (_isCurrent(generation) && AppState.shared.deviceMac == deviceMac) {
        update();
      }
    } catch (_) {
      if (_isCurrent(generation)) {
        AppState.shared.showToast("Failed to load AI Agent settings.");
      }
    }
  }

  Future<void> _loadAgentForDevice(
    bool isWelCome,
    int generation,
    String deviceMac,
  ) async {
    var devices = await XiaoZhiUtil.shared.getDevice(deviceMac);
    if (!_isCurrent(generation) || AppState.shared.deviceMac != deviceMac) {
      return;
    }

    if (devices.isEmpty) {
      agent = null;
      setDefaultCreateData();
      return;
    }

    var agentId = devices.first.agent_id;
    if (agentId == null) {
      final generateLicense = await XiaoZhiUtil.shared.generateLicense(
        deviceMac,
      );
      if (!_isCurrent(generation) || AppState.shared.deviceMac != deviceMac) {
        return;
      }
      if (generateLicense?.serialNumber == null) {
        AppState.shared.showToast("Failed to generate device license.");
        return;
      }

      final mac = MacAddressValidator.formatMac(deviceMac);
      if (mac == null) {
        AppState.shared.showToast("Failed to format device MAC address.");
        return;
      }

      final activated = await XiaoZhiUtil.shared.agentsDevicesActivate(
        generateLicense!.serialNumber!,
        mac,
      );
      if (!_isCurrent(generation) || AppState.shared.deviceMac != deviceMac) {
        return;
      }
      if (!activated) {
        AppState.shared.showToast("Device cloud activation failed.");
        return;
      }

      // Re-query once after activation. Do not recursively recreate
      // controllers/workers or retry forever when the backend is not ready.
      devices = await XiaoZhiUtil.shared.getDevice(deviceMac);
      if (!_isCurrent(generation) || AppState.shared.deviceMac != deviceMac) {
        return;
      }
      agentId = devices.isEmpty ? null : devices.first.agent_id;
      if (agentId == null) {
        AppState.shared.showToast(
          "Device activation is still being prepared. Please try again.",
        );
        return;
      }
    }

    final originalAgent = await XiaoZhiUtil.shared.getAgentDetail(agentId);
    if (!_isCurrent(generation) || AppState.shared.deviceMac != deviceMac) {
      return;
    }
    if (originalAgent == null) {
      AppState.shared.showToast("Failed to load the AI Agent.");
      return;
    }

    if (isWelCome) {
      final templates = await XiaoZhiUtil.shared.agentTemplatesList(1, 10);
      if (!_isCurrent(generation) || AppState.shared.deviceMac != deviceMac) {
        return;
      }
      if (templates.isNotEmpty) {
        final template = templates.first;
        agent = Agent(
          id: originalAgent.id,
          user_name: template.user_name,
          agent_name: template.agent_name,
          assistant_name: template.assistant_name,
          llm_model: template.llm_model,
          tts_voice: getTtsVoice("en", template.tts_voices ?? []),
          tts_speech_speed: template.tts_speech_speed,
          tts_pitch: template.tts_pitch,
          asr_speed: template.asr_speed,
          language: "en",
          character: template.character,
          memory: "",
          memory_type: "SHORT_TERM",
          knowledge_base_ids: template.knowledge_base_ids,
        );
      } else {
        agent = originalAgent;
      }
    } else {
      agent = originalAgent;
    }

    selectedMcpEndpoints.clear();
    fillEditData(agent!);
  }

  String getTtsVoice(String language, List<String> ttsVoices) {
    final String prefix = '$language:';
    for (final String voice in ttsVoices) {
      if (voice.startsWith(prefix)) {
        return voice.substring(prefix.length);
      }
    }
    if (ttsVoices.isNotEmpty) {
      for (final String voice in ttsVoices) {
        final int idx = voice.indexOf(':');
        if (idx != -1) {
          return voice.substring(idx + 1);
        }
      }
      return ttsVoices[0];
    }
    return '';
  }

  Future<void> loadTtsList(int generation) async {
    final loadedTtsData = await XiaoZhiUtil.shared.getTtsList();
    if (!_isCurrent(generation)) return;
    ttsData = loadedTtsData;
    if (ttsData?.ttsVoices != null) {
      languageList.value = ttsData!.ttsVoices!.keys.toList();
    }
    if (languageList.isNotEmpty && selectedLanguage.isEmpty) {
      selectedLanguage.value = languageList.first;
    }
    _updateTtsVoiceList(selectedLanguage.value);
  }

  void _updateTtsVoiceList(String lang) {
    if (_disposed) return;
    if (ttsData?.ttsVoices == null || lang.isEmpty) {
      ttsList.clear();
      selectedTtsVoice.value = null;
      return;
    }
    ttsList.value = ttsData!.ttsVoices![lang] ?? [];
    selectedTtsVoice.value = ttsList.isNotEmpty ? ttsList.first : null;
    update();
  }

  Future<void> loadModelList(int generation) async {
    final models = await XiaoZhiUtil.shared.getModelList();
    if (!_isCurrent(generation)) return;
    modelList.assignAll(models);
    update();
  }

  Future<void> loadCommonMcpTools(int generation) async {
    final tools = await XiaoZhiUtil.shared.getCommonMcpTool();
    if (!_isCurrent(generation)) return;
    commonMcpTools.value = tools;
    update();
  }

  String getContext(String? context, String defaultString) {
    if (context == null) {
      return defaultString;
    } else if (context.isEmpty) {
      return defaultString;
    } else {
      return context;
    }
  }

  void fillEditData(Agent agent) {
    agentNameController.text = getContext(
      agent.agent_name,
      "StackChan AI Agent",
    );
    assistantNameController.text = getContext(
      agent.assistant_name,
      "StackChan",
    );
    characterController.text = agent.character ?? "";
    memoryController.text = agent.memory ?? "";
    if (languageList.contains(agent.language)) {
      selectedLanguage.value = agent.language!;
    } else if (languageList.isNotEmpty) {
      selectedLanguage.value = languageList.first;
    }

    ttsSpeed.value = agent.tts_speech_speed ?? "normal";
    ttsPitch.value = agent.tts_pitch ?? 0;
    asrSpeed.value = agent.asr_speed ?? "normal";
    memoryType.value = agent.memory_type ?? "SHORT_TERM";

    if (agent.llm_model != null && agent.llm_model!.isNotEmpty) {
      selectedModel.value = modelList.firstWhereOrNull(
        (m) => m.name == agent.llm_model,
      );
    } else {
      for (var i in modelList) {
        if (i.name != null) {
          if (i.name!.toLowerCase() == "qwen") {
            selectedModel.value = i;
            update();
            break;
          } else if (i.name!.toLowerCase().contains("qwen")) {
            selectedModel.value = i;
            update();
            break;
          }
        }
      }
    }

    if (agent.tts_voice != null) {
      selectedTtsVoice.value = ttsList.firstWhereOrNull(
        (t) => t.voiceId == agent.tts_voice,
      );
    }

    if (agent.mcp_endpoints != null) {
      selectedMcpEndpoints.addAll(agent.mcp_endpoints!);
    }
    update();
  }

  void setDefaultCreateData() {
    agentNameController.text = "My AI Agent";
    assistantNameController.text = "StackChan";
    ttsSpeed.value = "normal";
    ttsPitch.value = 0;
    asrSpeed.value = "normal";
    memoryType.value = "SHORT_TERM";
    if (modelList.isNotEmpty) selectedModel.value = modelList.first;
    update();
  }

  void toggleMcpTool(String? endpointId) {
    if (endpointId == null) return;
    selectedMcpEndpoints.contains(endpointId)
        ? selectedMcpEndpoints.remove(endpointId)
        : selectedMcpEndpoints.add(endpointId);
    update();
  }

  Future<bool> submitAgent() async {
    if (_disposed || isLoading.value) return false;
    if (assistantNameController.text.trim().isEmpty) {
      AppState.shared.showToast("Please input assistant assistant name.");
      return false;
    }
    if (selectedModel.value == null) {
      AppState.shared.showToast("Please select an LLM Model.");
      return false;
    }
    if (selectedTtsVoice.value == null) {
      AppState.shared.showToast("Please select a voice tone.");
      return false;
    }
    final generation = _lifecycleGeneration;
    isLoading.value = true;
    try {
      final agentParams = AgentCreate(
        agent_name: agent?.agent_name ?? "StackChan AI Agent",
        assistant_name: assistantNameController.text.trim(),
        llm_model: selectedModel.value!.name!,
        tts_voice: selectedTtsVoice.value!.voiceId!,
        tts_speech_speed: ttsSpeed.value,
        tts_pitch: ttsPitch.value,
        asr_speed: asrSpeed.value,
        language: selectedLanguage.value,
        character: characterController.text.trim(),
        memory: memoryController.text.trim(),
        memory_type: memoryType.value,
        mcp_endpoints: null,
        product_mcp_endpoints: null,
      );

      bool result;
      if (agent != null) {
        result = await XiaoZhiUtil.shared.updateAgent(agent!.id!, agentParams);
      } else {
        final agentId = await XiaoZhiUtil.shared.createAgent(agentParams);
        result = agentId != null;
      }

      if (!_isCurrent(generation)) return false;
      if (result) {
        AppState.shared.showToast(
          agent != null
              ? "Agent edited successfully"
              : "Agent created successfully",
        );
      }
      return result;
    } catch (_) {
      if (_isCurrent(generation)) {
        AppState.shared.showToast("Failed to save AI Agent settings.");
      }
      return false;
    } finally {
      if (_isCurrent(generation)) {
        isLoading.value = false;
      }
    }
  }

  @override
  void onClose() {
    if (_disposed) return;
    _disposed = true;
    ++_lifecycleGeneration;
    _languageWorker?.dispose();
    _languageWorker = null;
    agentNameController.dispose();
    assistantNameController.dispose();
    characterController.dispose();
    memoryController.dispose();
    super.onClose();
  }
}

class _XiaoZhiWelcomePageState extends State<XiaoZhiWelcomePage> {
  String getLanguagesTitle(String lg) {
    if (ValueConstant.languages[lg] != null) {
      return ValueConstant.languages[lg]!;
    } else {
      return lg;
    }
  }

  String getSpeedText(String speed) {
    switch (speed) {
      case "slow":
        return "Slow";
      case "fast":
        return "Fast";
      default:
        return "Normal";
    }
  }

  String getMemoryText(String type) {
    switch (type) {
      case "OFF":
        return "Off";
      case "SHORT_TERM":
        return "Short Term";
      default:
        return type;
    }
  }

  XiaoZhiEditAgentModel model = XiaoZhiEditAgentModel();

  @override
  void initState() {
    super.initState();
    init();
  }

  void init() {
    unawaited(model.initPageData(widget.isWelCome ?? false));
  }

  Future<void> _stopVoicePreview() async {
    try {
      await MusicUtil.shared.stopMusic();
    } catch (_) {
      // Leaving the page must continue even if the preview player has failed.
    }
  }

  Future<void> _playVoicePreview(String? url) async {
    try {
      await MusicUtil.shared.playUrlMusicOnce(
        url,
        completion: () {
          if (mounted) {
            setState(() {});
          }
        },
      );
    } catch (_) {
      if (mounted) {
        AppState.shared.showToast("Failed to play the voice preview.");
      }
    }
  }

  @override
  void dispose() {
    unawaited(_stopVoicePreview());
    FocusManager.instance.primaryFocus?.unfocus();
    model.onClose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return CupertinoPageScaffold(
      backgroundColor: CupertinoColors.systemGroupedBackground.resolveFrom(
        context,
      ),
      child: GestureDetector(
        onTap: () {
          if (mounted) {
            FocusScope.of(context).unfocus();
          }
        },
        behavior: .opaque,
        child: Column(
          children: [
            Expanded(
              child: CustomScrollView(
                slivers: [
                  CupertinoSliverNavigationBar(
                    largeTitle: Text("Agent Setting"),
                    trailing: widget.isWelCome != true
                        ? CupertinoButton(
                            padding: .zero,
                            child: Icon(
                              CupertinoIcons.xmark_circle_fill,
                              size: 25,
                              color: CupertinoColors.separator.resolveFrom(
                                context,
                              ),
                            ),
                            onPressed: () {
                              CupertinoSheetRoute.popSheet(context);
                            },
                          )
                        : SizedBox.shrink(),
                  ),
                  SliverList.list(
                    children: [
                      Image.asset(
                        "assets/lateral_image.png",
                        width: double.infinity,
                        height: 120,
                        fit: BoxFit.contain,
                      ),
                      SizedBox(height: 15),

                      //info
                      CupertinoListSection.insetGrouped(
                        header: Row(
                          mainAxisSize: .min,
                          spacing: 10,
                          children: [
                            Text("Assistant Name"),
                            Text(
                              "This is what Agent calls itself",
                              style: TextStyle(
                                fontSize: 10,
                                color: CupertinoColors.separator.resolveFrom(
                                  context,
                                ),
                              ),
                            ),
                          ],
                        ),
                        children: [
                          CupertinoListTile(
                            padding: .only(left: 10, right: 10),
                            title: CupertinoTextField(
                              maxLength: 30,
                              decoration: BoxDecoration(),
                              controller: model.assistantNameController,
                              placeholder: "Enter assistant name",
                              clearButtonMode: OverlayVisibilityMode.editing,
                            ),
                          ),
                        ],
                      ),

                      //languageset
                      CupertinoListSection.insetGrouped(
                        header: Text("Language"),
                        children: [
                          Obx(
                            () => buildSelectItem(
                              title: getLanguagesTitle(
                                model.selectedLanguage.value,
                              ),
                              value: Icon(
                                CupertinoIcons.right_chevron,
                                size: 16,
                              ),
                              items: model.languageList
                                  .map((value) => getLanguagesTitle(value))
                                  .toList(),
                              onTapIndex: (index) {
                                model.selectedLanguage.value =
                                    model.languageList[index];
                              },
                              selectedValue: getLanguagesTitle(
                                model.selectedLanguage.value,
                              ),
                            ),
                          ),
                        ],
                      ),

                      //LLMmodel
                      CupertinoListSection.insetGrouped(
                        header: Text("LLM Model"),
                        children: [
                          Obx(
                            () => buildSelectItem(
                              title:
                                  model.selectedModel.value?.description
                                      .regularExpressionSubstitution() ??
                                  "Select model",
                              value: Icon(
                                CupertinoIcons.right_chevron,
                                size: 16,
                              ),
                              items: model.modelList
                                  .map(
                                    (e) =>
                                        e.description
                                            .regularExpressionSubstitution() ??
                                        "",
                                  )
                                  .toList(),
                              onTapIndex: (index) {
                                model.selectedModel.value =
                                    model.modelList[index];
                              },
                              selectedValue: model
                                  .selectedModel
                                  .value
                                  ?.description
                                  .regularExpressionSubstitution(),
                            ),
                          ),
                        ],
                      ),
                      //TTSset
                      CupertinoListSection.insetGrouped(
                        header: Text("Voice Settings"),
                        children: [
                          //voice tone
                          Obx(() => ttsVoiceWidget()),
                          //translated comment
                          Obx(
                            () => buildSelectItem(
                              title: "Speed :",
                              value: Row(
                                mainAxisSize: .min,
                                spacing: 15,
                                children: [
                                  Text(getSpeedText(model.ttsSpeed.value)),
                                  Icon(CupertinoIcons.right_chevron, size: 16),
                                ],
                              ),
                              items: model.speedList
                                  .map((e) => getSpeedText(e))
                                  .toList(),
                              onTapIndex: (index) {
                                model.ttsSpeed.value = model.speedList[index];
                              },
                              selectedValue: getSpeedText(model.ttsSpeed.value),
                            ),
                          ),
                          //translated comment
                          Obx(
                            () => buildSelectItem(
                              title: "Pitch :",
                              value: Row(
                                mainAxisSize: .min,
                                spacing: 15,
                                children: [
                                  Text(model.ttsPitch.value.toString()),
                                  Icon(CupertinoIcons.right_chevron, size: 16),
                                ],
                              ),
                              items: model.pitchList
                                  .map((e) => e.toString())
                                  .toList(),
                              onTapIndex: (index) {
                                model.ttsPitch.value = model.pitchList[index];
                              },
                              selectedValue: model.ttsPitch.value.toString(),
                            ),
                          ),
                          //ASR
                          Obx(
                            () => buildSelectItem(
                              title: "ASR Speed :",
                              value: Row(
                                mainAxisSize: .min,
                                spacing: 15,
                                children: [
                                  Text(getSpeedText(model.asrSpeed.value)),
                                  Icon(CupertinoIcons.right_chevron, size: 16),
                                ],
                              ),
                              items: model.speedList
                                  .map((e) => getSpeedText(e))
                                  .toList(),
                              onTapIndex: (index) {
                                model.asrSpeed.value = model.speedList[index];
                              },
                              selectedValue: getSpeedText(model.asrSpeed.value),
                            ),
                          ),
                        ],
                      ),

                      //with
                      CupertinoListSection.insetGrouped(
                        header: Text("Personality"),
                        children: [
                          CupertinoListTile(
                            padding: .all(15),
                            title: CupertinoTextField(
                              padding: .all(15),
                              controller: model.characterController,
                              placeholder: "Set agent character",
                              maxLines: 10,
                              clearButtonMode: OverlayVisibilityMode.editing,
                              decoration: BoxDecoration(
                                color: CupertinoColors.systemGroupedBackground
                                    .resolveFrom(context),
                                borderRadius: .circular(15),
                              ),
                            ),
                          ),
                        ],
                      ),
                      CupertinoListSection.insetGrouped(
                        header: Text("Memory"),
                        children: [
                          Obx(
                            () => buildSelectItem(
                              title: "",
                              value: Row(
                                mainAxisSize: .min,
                                spacing: 15,
                                children: [
                                  Text(getMemoryText(model.memoryType.value)),
                                  Icon(CupertinoIcons.right_chevron, size: 16),
                                ],
                              ),
                              items: model.memoryTypeList
                                  .map((e) => getMemoryText(e))
                                  .toList(),
                              onTapIndex: (index) {
                                model.memoryType.value =
                                    model.memoryTypeList[index];
                              },
                              selectedValue: getMemoryText(
                                model.memoryType.value,
                              ),
                            ),
                          ),
                          CupertinoListTile(
                            padding: .all(15),
                            title: CupertinoTextField(
                              controller: model.memoryController,
                              placeholder: "Set memory content",
                              maxLines: 10,
                              padding: .all(15),
                              decoration: BoxDecoration(
                                color: CupertinoColors.systemGroupedBackground
                                    .resolveFrom(context),
                                borderRadius: .circular(15),
                              ),
                              clearButtonMode: OverlayVisibilityMode.editing,
                            ),
                          ),
                        ],
                      ),

                      // MCP Tools
                      // CupertinoListSection.insetGrouped(
                      //   header: Text("MCP Tools"),
                      //   children: model.commonMcpTools.isNotEmpty
                      //       ? model.commonMcpTools.map((tool) {
                      //           final isSelected = model.selectedMcpEndpoints
                      //               .contains(tool.endpoint_id);
                      //           return CupertinoListTile(
                      //             onTap: () {
                      //               model.toggleMcpTool(tool.endpoint_id);
                      //               setState(() {});
                      //             },
                      //             title: Text(tool.name ?? ""),
                      //             trailing: isSelected
                      //                 ? Icon(
                      //                     CupertinoIcons.check_mark,
                      //                     size: 20,
                      //                   )
                      //                 : null,
                      //           );
                      //         }).toList()
                      //       : [
                      //           CupertinoListTile(
                      //             title: Center(
                      //               child: Text(
                      //                 "There is currently no MCP tool",
                      //               ),
                      //             ),
                      //           ),
                      //         ],
                      // ),
                      SizedBox(height: 20),
                    ],
                  ),
                ],
              ),
            ),

            //Bottombutton
            Padding(
              padding: .only(
                left: 15,
                right: 15,
                top: 15,
                bottom: MediaQuery.paddingOf(context).bottom + 15,
              ),
              child: Obx(
                () => model.isLoading.value
                    ? CupertinoActivityIndicator(radius: 16)
                    : widget.isWelCome == true
                    ? Row(
                        spacing: 15,
                        children: [
                          Expanded(
                            child: CupertinoButton.tinted(
                              child: Text("Skip"),
                              onPressed: () {
                                if (mounted) {
                                  FocusScope.of(context).unfocus();
                                }
                                Navigator.of(context).push(
                                  CupertinoPageRoute(
                                    builder: (context) {
                                      return DeviceWifiConfig(isWelCome: true);
                                    },
                                  ),
                                );
                              },
                            ),
                          ),
                          Expanded(
                            child: CupertinoButton.filled(
                              child: Text("Continue"),
                              onPressed: () async {
                                if (mounted) {
                                  FocusScope.of(context).unfocus();
                                }
                                final res = await model.submitAgent();
                                if (res && mounted) {
                                  Navigator.push(
                                    this.context,
                                    CupertinoPageRoute(
                                      builder: (_) =>
                                          DeviceWifiConfig(isWelCome: true),
                                    ),
                                  );
                                }
                              },
                            ),
                          ),
                        ],
                      )
                    : CupertinoButton.filled(
                        child: SizedBox(
                          width: .infinity,
                          child: Center(child: Text("Save")),
                        ),
                        onPressed: () async {
                          if (mounted) {
                            FocusScope.of(context).unfocus();
                          }
                          final result = await model.submitAgent();
                          if (result && mounted) {
                            showCupertinoDialog(
                              barrierDismissible: false,
                              context: this.context,
                              builder: (context) {
                                return CupertinoAlertDialog(
                                  title: Text(
                                    "AI agent config has been saved successfully, and will be active after manually restarting the device.",
                                  ),
                                  actions: [
                                    CupertinoDialogAction(
                                      onPressed: () {
                                        Navigator.of(context).pop();
                                        Navigator.pop(this.context);
                                      },
                                      isDefaultAction: true,
                                      child: Text("Confirm"),
                                    ),
                                  ],
                                );
                              },
                            );
                          }
                        },
                      ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget ttsVoiceWidget() {
    return PullDownButton(
      useRootNavigator: true,
      itemBuilder: (context) => model.ttsList
          .map(
            (e) => PullDownMenuItem.selectable(
              onTap: () {
                model.selectedTtsVoice.value = e;
              },
              title: e.voiceName ?? "",
              selected: model.selectedTtsVoice.value?.voiceId == e.voiceId,
              iconWidget: e.voiceDemo != null
                  ? CupertinoButton(
                      padding: .zero,
                      child: Icon(CupertinoIcons.speaker_2),
                      onPressed: () {
                        unawaited(_playVoicePreview(e.voiceDemo));
                      },
                    )
                  : null,
            ),
          )
          .toList(),
      buttonBuilder: (context, showMenu) => CupertinoButton(
        padding: EdgeInsets.zero,
        foregroundColor: CupertinoColors.systemGrey.resolveFrom(context),
        minimumSize: .zero,
        onPressed: showMenu,
        pressedOpacity: 1,
        child: CupertinoListTile(
          title: Text("Voice :"),
          trailing: Row(
            mainAxisSize: .min,
            spacing: 15,
            children: [
              Text(model.selectedTtsVoice.value?.voiceName ?? "Select voice"),
              Icon(CupertinoIcons.right_chevron, size: 16),
            ],
          ),
          leading: null,
        ),
      ),
    );
  }

  Widget buildSelectItem({
    required String title,
    required Widget value,
    required List<String> items,
    required Function(int) onTapIndex,
    required String? selectedValue,
  }) {
    return PullDownButton(
      useRootNavigator: true,
      itemBuilder: (context) => items.indexed
          .map(
            (item) => PullDownMenuItem.selectable(
              onTap: () {
                onTapIndex(item.$1);
              },
              title: item.$2,
              selected: item.$2 == selectedValue,
            ),
          )
          .toList(),
      buttonBuilder: (context, showMenu) {
        return CupertinoButton(
          padding: EdgeInsets.zero,
          foregroundColor: CupertinoColors.systemGrey.resolveFrom(context),
          minimumSize: .zero,
          onPressed: showMenu,
          pressedOpacity: 1,
          child: CupertinoListTile(
            title: Text(title),
            trailing: value,
            leading: null,
          ),
        );
      },
    );
  }
}
