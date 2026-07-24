#pragma once

class ConversationManager;

namespace conversation_runtime {

void prepare();
void start();
ConversationManager* manager();

}  // namespace conversation_runtime
