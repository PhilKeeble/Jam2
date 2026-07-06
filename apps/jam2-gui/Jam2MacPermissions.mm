#include "Jam2MacPermissions.hpp"

#import <AVFoundation/AVFoundation.h>
#import <dispatch/dispatch.h>

bool jam2EnsureMicrophonePermission(QString* errorMessage)
{
    const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusAuthorized) {
        return true;
    }
    if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "Microphone access is denied for Jam2. Enable it in System Settings > Privacy & Security > Microphone, then restart Jam2.");
        }
        return false;
    }

    __block BOOL granted = NO;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL accessGranted) {
        granted = accessGranted;
        dispatch_semaphore_signal(semaphore);
    }];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    if (!granted && errorMessage != nullptr) {
        *errorMessage = QStringLiteral(
            "Microphone access was not granted. Jam2 needs microphone access to send or record input audio.");
    }
    return granted == YES;
}
