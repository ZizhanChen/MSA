#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/limits.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "MultiScreenManager.h"

namespace android {

MultiScreenManager::MultiScreenManager() {}

MultiScreenManager::~MultiScreenManager() {}


Rect& MultiScreenManager::getViewport(int id) const {
    return mCombinedScreen->mViewports[id]; 
}

int MultiScreenManager::registerScreen(int id, int width, int height){
    mCombinedScreen->mHeight += height; 
    int left, top, right, bottom; 
    if (id == 0) {
        left = 0;
        top = 0;
        right = mCombinedScreen->mWidth;
        bottom = mCombinedScreen->mHeight/2;
    } else {
        left = 0;
        top = mCombinedScreen->mHeight/2;
        right = mCombinedScreen->mWidth;
        bottom = mCombinedScreen->mHeight;
    }
    viewport = Rect(left, top, right, bottom); 
    mCombinedScreen->mViewports.insert(std::pair<int, Rect> (id, viewport));
    return 0;  
}

MultiScreenManager::CombinedScreen::CombinedScreen(int number, int width, int height)
    : mScreenNumber(number), 
      mWidth(width), 
      mHeight(height) {}

MultiScreenManager::CombinedScreen::~CombinedScreen() {}

}; // namespace android