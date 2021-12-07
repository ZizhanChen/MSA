#ifndef _MULTI_SCREEN_MANAGER_H
#define _MULTI_SCREEN_MANAGER_H

#include <stdlib.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <android/native_window.h>
#include <binder/IBinder.h>
#include <gui/LayerState.h>
#include <hardware/hwcomposer_defs.h>
#include <math/mat4.h>
#include <renderengine/RenderEngine.h>
#include <system/window.h>
#include <ui/GraphicTypes.h>
#include <ui/HdrCapabilities.h>
#include <ui/Region.h>
#include <ui/Transform.h>
#include <utils/Mutex.h>
#include <utils/RefBase.h>
#include <utils/Timers.h>
#include <utils/Log.h>
#include <utils/List.h>
#include <utils/Errors.h>
#include <utils/PropertyMap.h>
#include <utils/KeyedVector.h>
#include <utils/BitSet.h>

namespace android {
class MultiScreenManager {
public: 
    MultiScreenManager();
    virtual uint32_t getSreenNumber() const;
    virtual bool isSplitModeEnabled() const; 
    virtual Rect& getViewport() const;
    virtual int registerScreen() const;  

protected:
    virtual ~MultiScreenManager();

private:
    const sp<const CombinedScreen> mCombinedScreen; 
};

class CombinedScreen {
    int mScreenNumber; 
    int mWidth; 
    int mHeight;
    std::map<int, Rect> mViewports;
    CombinedScreen(int number, int width, int height);
    ~CombinedScreen();
};

}; // namespace android

#endif // _MULTI_SCREEN_MANAGER_H
