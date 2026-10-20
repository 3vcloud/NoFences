// NoFences: desktop fences that hold shortcuts to files, folders and links. Each fence is a window whose page is in frontend/.

#include <cstdlib>
#include <exception>
#include <filesystem>

#include "FenceApp.h"
#include "Platform.h"
#include "WebViewWindow.h"

int main() {
    try {
        // NOFENCES_DATA_DIR keeps a test run's fences apart from the real ones.
        const char* override = std::getenv("NOFENCES_DATA_DIR");
        const std::filesystem::path dataDirectory =
            override && *override ? nofences::pathFromUtf8(override) : nofences::defaultDataDirectory();
        std::filesystem::create_directories(dataDirectory);
        // A second start would open the same fences again, so it ends here while the first keeps running.
        if (!nofences::claimSingleInstance(dataDirectory)) {
            return 0;
        }
        setApplicationName("NoFences");
        initializeWindowing();
        nofences::FenceApp app{nofences::FenceStore(dataDirectory)};
        app.start();
        runEventLoop();
    } catch (const RuntimeMissingError&) {
        // The dialog has already said what's missing.
        return 1;
    } catch (const std::exception& error) {
        showError(error.what());
        return 1;
    }
    return 0;
}
