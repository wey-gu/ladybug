#include "processor/operator/simple/load_extension.h"

#include "extension/extension_manager.h"
#include "main/client_context.h"
#include "main/database.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"

using namespace lbug::common;

namespace lbug {
namespace processor {

using namespace lbug::extension;

std::string LoadExtensionPrintInfo::toString() const {
    return "Load " + extensionName;
}

void LoadExtension::executeInternal(ExecutionContext* context) {
    auto clientContext = context->clientContext;
    auto extensionManager = extension::ExtensionManager::Get(*clientContext);
    if (ExtensionUtils::isOfficialExtension(path) &&
        extensionManager->isStaticLinkedExtension(path, clientContext)) {
        appendMessage(stringFormat("Extension {} is already static linked with kuzu core.", path),
            context->clientContext->getDatabase()->getMemoryManager());
        return;
    }
    ExtensionManager::Get(*clientContext)->loadExtension(path, clientContext);
    appendMessage(stringFormat("Extension: {} has been loaded.", path),
        storage::MemoryManager::Get(*clientContext));
}

} // namespace processor
} // namespace lbug
