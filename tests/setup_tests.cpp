#include "setup.h"
#include <iostream>

using namespace contour::app;
void require(bool ok,const char* reason) { if (!ok) { throw std::runtime_error(reason); } }
int main() {
    try {
        const std::wstring endpoint=L"{0.0.0.00000000}.{d673b998-8476-4faa-af56-e3eec4688246}";
        require(valid_setup_endpoint(endpoint),"Valid playback endpoint rejected");
        for (const auto& invalid : {L"",L"contour-offline-stereo",L"{0.0.1.00000000}.{d673b998-8476-4faa-af56-e3eec4688246}",
            L"{0.0.0.00000000}.{d673b998-8476-4faa-af56-e3eec468824Z}",L"output\" -Action Remove"}) {
            require(!valid_setup_endpoint(invalid),"Unsafe or non-playback endpoint accepted");
        }
        const auto args=setup_arguments(L"C:\\Program Files\\Contour\\setup\\contour-setup.ps1",endpoint,"Inspect");
        require(args.find(L"-File \"C:\\Program Files\\Contour\\setup\\contour-setup.ps1\"")!=args.npos,"Spaced path was not quoted");
        bool rejected=false;
        try { setup_arguments(L"anything",endpoint,"Install -Command evil"); } catch (...) { rejected=true; }
        require(rejected,"Unknown action accepted");
        Json report={{"endpoint",utf8(endpoint)},{"busy",false},{"canInstall",true},{"canRemove",false}};
        require(setup_action_allowed(report,utf8(endpoint),"Install"),"Reviewed install rejected");
        require(!setup_action_allowed(report,"different endpoint","Install"),"Review carried across output change");
        require(!setup_action_allowed(report,utf8(endpoint),"Remove"),"Unreviewed removal accepted");
        report["busy"]=true;
        require(!setup_action_allowed(report,utf8(endpoint),"Install"),"Concurrent setup accepted");
        std::cout<<"PASS setup boundary: playback identities, fixed actions, quoting, stale review, busy guard\n";
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
