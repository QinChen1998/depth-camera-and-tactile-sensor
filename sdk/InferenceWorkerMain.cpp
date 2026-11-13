#include "../sensors/RvAssistDepth.h"
#include "../sensors/RvInferenceRuntime.h"
#include <QCoreApplication>

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    const auto command = args.value(1);
    if (command == "--rv-assist-worker" && args.size() == 5 && RvAssistDepth::supportsHostModel(args[4]))
        return runRvAssistWorker(args[2], args[3], args[4]);
    if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") != "1") return 3;
    if (command == "--inference-enumerate" && args.size() == 3)
        return RvInferenceRuntime::writeEnumeration(args[2]);
    if (command == "--inference-probe" && args.size() == 4)
        return runRvInferenceProbe(args[2], args[3]);
    return 3;
}
