// iOS document provider — UIDocumentPickerViewController + security-scoped URLs.
// This file is Objective-C++ (.mm) to call UIKit from C++ code.

#include <uapmd-file/IDocumentProvider.hpp>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// ─── Utilities (file-scope, used by delegate implementations below) ──────────

using namespace uapmd;

namespace {

// Obtain the topmost presented view controller from the active window scene.
// This is used as the presenter for UIDocumentPickerViewController.
UIViewController* topViewController()
{
    UIWindowScene* activeScene = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:[UIWindowScene class]] &&
            scene.activationState == UISceneActivationStateForegroundActive) {
            activeScene = (UIWindowScene*)scene;
            break;
        }
    }
    if (!activeScene)
        return nil;

    UIViewController* vc = activeScene.windows.firstObject.rootViewController;
    while (vc.presentedViewController)
        vc = vc.presentedViewController;
    return vc;
}

// Build the UTType array from DocumentFilter mime_types.
// Falls back to UTTypeItem (any) when no recognisable types are found.
NSArray<UTType*>* utTypesFromFilters(const std::vector<DocumentFilter>& filters)
{
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (auto& f : filters) {
        for (auto& mime : f.mime_types) {
            UTType* t = [UTType typeWithMIMEType:[NSString stringWithUTF8String:mime.c_str()]];
            if (t) [types addObject:t];
        }
    }
    if (types.count == 0)
        [types addObject:UTTypeItem];
    return types;
}

// Start security-scoped access and return whether it was needed.
// Must be balanced with stopAccess() if this returns YES.
BOOL startAccess(NSURL* url) { return [url startAccessingSecurityScopedResource]; }
void  stopAccess (NSURL* url, BOOL started) { if (started) [url stopAccessingSecurityScopedResource]; }

// Read all bytes from a security-scoped URL.
std::vector<uint8_t> readBytes(NSURL* url)
{
    NSData* data = [NSData dataWithContentsOfURL:url];
    if (!data) return {};
    const uint8_t* ptr = static_cast<const uint8_t*>(data.bytes);
    return {ptr, ptr + data.length};
}

// Write bytes to a URL (creates or replaces the file).
bool writeBytes(NSURL* url, const std::vector<uint8_t>& data)
{
    NSData* nsData = [NSData dataWithBytes:data.data() length:data.size()];
    NSError* err = nil;
    [nsData writeToURL:url options:NSDataWritingAtomic error:&err];
    return err == nil;
}

// Build a DocumentHandle from a security-scoped NSURL.
DocumentHandle handleFromURL(NSURL* url)
{
    DocumentHandle h;
    h.id           = url.absoluteString.UTF8String;
    h.display_name = url.lastPathComponent.UTF8String;
    // Best-effort MIME type via UTType
    NSString* uti = nil;
    [url getResourceValue:&uti forKey:NSURLTypeIdentifierKey error:nil];
    if (uti) {
        UTType* type = [UTType typeWithIdentifier:uti];
        h.mime_type = type.preferredMIMEType.UTF8String ?: "";
    }
    return h;
}

NSURL* urlFromHandle(const DocumentHandle& handle)
{
    return [NSURL URLWithString:[NSString stringWithUTF8String:handle.id.c_str()]];
}

} // anonymous namespace

// ─── Delegate objects (must be at global scope — ObjC classes cannot be inside a C++ namespace) ───

// Shared delegate for open picks.
@interface UAPMDOpenPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (copy) void (^onPick)(NSArray<NSURL*>*);
@property (copy) void (^onCancel)(void);
@end

@implementation UAPMDOpenPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
  didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if (self.onPick) self.onPick(urls);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    if (self.onCancel) self.onCancel();
}
@end

// Shared delegate for export (save) picks.
@interface UAPMDExportPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (copy) void (^onExport)(NSURL*);  // destination URL
@property (copy) void (^onCancel)(void);
@end

@implementation UAPMDExportPickerDelegate
- (void)documentPicker:(UIDocumentPickerViewController*)controller
  didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if (self.onExport && urls.count > 0) self.onExport(urls.firstObject);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    if (self.onCancel) self.onCancel();
}
@end

// ─── DocumentProviderIOS ─────────────────────────────────────────────────────

namespace uapmd {

class DocumentProviderIOS final : public IDocumentProvider {
    // Strong references to delegates so they live as long as the picker is on screen.
    // Protected by mutex_ since tick() and pick callbacks may run concurrently.
    std::mutex mutex_;
    // Delegates are kept alive as __strong Obj-C objects bridged to void*.
    // Stored as CFTypeRef (retained) and released when the pick completes.
    void* openDelegate_  = nullptr;
    void* exportDelegate_ = nullptr;

    void retainDelegate(void** slot, id obj) {
        if (*slot) CFRelease(*slot);
        *slot = obj ? (void*)CFBridgingRetain(obj) : nullptr;
    }
    void releaseDelegate(void** slot) {
        if (*slot) { CFRelease(*slot); *slot = nullptr; }
    }

public:
    ~DocumentProviderIOS() override {
        releaseDelegate(&openDelegate_);
        releaseDelegate(&exportDelegate_);
    }

    // ── Picking ───────────────────────────────────────────────────────────────

    void pickOpenDocuments(
        std::vector<DocumentFilter> filters,
        bool allowMultiple,
        PickCallback callback) override
    {
        dispatch_async(dispatch_get_main_queue(), ^{
            UIViewController* presenter = topViewController();
            if (!presenter) {
                callback({false, {}, "No active view controller to present from"});
                return;
            }

            NSArray<UTType*>* types = utTypesFromFilters(filters);
            UIDocumentPickerViewController* picker =
                [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types];
            picker.allowsMultipleSelection = allowMultiple ? YES : NO;
            picker.shouldShowFileExtensions = YES;

            UAPMDOpenPickerDelegate* delegate = [[UAPMDOpenPickerDelegate alloc] init];

            delegate.onPick = ^(NSArray<NSURL*>* urls) {
                DocumentPickResult result;
                result.success = true;
                for (NSURL* url in urls)
                    result.handles.push_back(handleFromURL(url));
                // Dispatch callback to calling/main thread
                dispatch_async(dispatch_get_main_queue(), ^{ callback(result); });
                std::lock_guard<std::mutex> lock(mutex_);
                releaseDelegate(&openDelegate_);
            };
            delegate.onCancel = ^{
                dispatch_async(dispatch_get_main_queue(), ^{
                    callback({true, {}, ""}); // success=true, zero handles = cancelled
                });
                std::lock_guard<std::mutex> lock(mutex_);
                releaseDelegate(&openDelegate_);
            };

            picker.delegate = delegate;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                retainDelegate(&openDelegate_, delegate);
            }
            [presenter presentViewController:picker animated:YES completion:nil];
        });
    }

    void pickSaveDocument(
        std::string defaultName,
        std::vector<DocumentFilter> filters,
        PickCallback callback) override
    {
        // iOS save flow:
        //   1. Create a temp file with defaultName in the app's tmp directory.
        //   2. Present UIDocumentPickerViewController in "export" mode so the
        //      user chooses where to save it.
        //   3. The returned handle points at the *exported* destination URL.
        //      The caller subsequently calls writeDocument() to fill it.
        //
        // Note: The temp stub file is empty; writeDocument() overwrites it at
        // the destination.  This matches the IDocumentProvider contract where
        // pick comes before write.

        dispatch_async(dispatch_get_main_queue(), ^{
            UIViewController* presenter = topViewController();
            if (!presenter) {
                callback({false, {}, "No active view controller to present from"});
                return;
            }

            // Create an empty stub file in tmp so the picker has something to export.
            NSURL* tmpDir  = [NSURL fileURLWithPath:NSTemporaryDirectory()];
            NSString* name = [NSString stringWithUTF8String:defaultName.c_str()];
            NSURL* stubURL = [tmpDir URLByAppendingPathComponent:name];
            [[NSData data] writeToURL:stubURL atomically:YES];

            UIDocumentPickerViewController* picker =
                [[UIDocumentPickerViewController alloc] initForExportingURLs:@[stubURL]
                                                                      asCopy:YES];
            picker.shouldShowFileExtensions = YES;

            UAPMDExportPickerDelegate* delegate = [[UAPMDExportPickerDelegate alloc] init];

            delegate.onExport = ^(NSURL* destURL) {
                DocumentPickResult result;
                result.success = true;
                result.handles.push_back(handleFromURL(destURL));
                dispatch_async(dispatch_get_main_queue(), ^{ callback(result); });
                std::lock_guard<std::mutex> lock(mutex_);
                releaseDelegate(&exportDelegate_);
            };
            delegate.onCancel = ^{
                dispatch_async(dispatch_get_main_queue(), ^{
                    callback({true, {}, ""}); // cancelled
                });
                std::lock_guard<std::mutex> lock(mutex_);
                releaseDelegate(&exportDelegate_);
            };

            picker.delegate = delegate;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                retainDelegate(&exportDelegate_, delegate);
            }
            [presenter presentViewController:picker animated:YES completion:nil];
        });
    }

    // ── I/O ───────────────────────────────────────────────────────────────────

    void readDocument(DocumentHandle handle, ReadCallback callback) override
    {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSURL* url = urlFromHandle(handle);
            BOOL access = startAccess(url);
            auto bytes = readBytes(url);
            stopAccess(url, access);

            DocumentIOResult result;
            if (bytes.empty() && !handle.id.empty()) {
                result.success = false;
                result.error   = "Failed to read: " + handle.id;
            } else {
                result.success = true;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                callback(result, bytes);
            });
        });
    }

    void writeDocument(DocumentHandle handle, std::vector<uint8_t> data,
                       WriteCallback callback) override
    {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSURL* url = urlFromHandle(handle);
            BOOL access = startAccess(url);
            bool ok = writeBytes(url, data);
            stopAccess(url, access);

            DocumentIOResult result;
            result.success = ok;
            if (!ok) result.error = "Failed to write: " + handle.id;
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result); });
        });
    }

    // ── Path bridging ─────────────────────────────────────────────────────────

    // iOS security-scoped URLs resolve directly to filesystem paths — no temp
    // files needed, unlike Android's content:// URIs.
    void resolveToPath(DocumentHandle handle, PathCallback callback) override
    {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSURL* url = urlFromHandle(handle);
            BOOL access = startAccess(url);
            std::filesystem::path path{url.fileSystemRepresentation};
            stopAccess(url, access);

            DocumentIOResult result;
            result.success = !path.empty();
            if (!result.success) result.error = "Cannot resolve path for: " + handle.id;
            dispatch_async(dispatch_get_main_queue(), ^{
                callback(result, path);
            });
        });
    }

    // ── Persistence ───────────────────────────────────────────────────────────

    // Uses security-scoped URL bookmark data, base64-encoded, for durable storage
    // across app launches (survives process restart, unlike plain URL strings).
    std::string persistHandle(const DocumentHandle& handle) override
    {
        NSURL* url = urlFromHandle(handle);
        BOOL access = startAccess(url);
        NSError* err = nil;
        NSData* bookmark = [url bookmarkDataWithOptions:0
                              includingResourceValuesForKeys:nil
                                             relativeToURL:nil
                                                     error:&err];
        stopAccess(url, access);
        if (!bookmark) return "";
        return [bookmark base64EncodedStringWithOptions:0].UTF8String;
    }

    std::optional<DocumentHandle> restoreHandle(const std::string& token) override
    {
        if (token.empty()) return std::nullopt;
        NSData* bookmark = [[NSData alloc]
            initWithBase64EncodedString:[NSString stringWithUTF8String:token.c_str()]
                               options:0];
        if (!bookmark) return std::nullopt;

        BOOL isStale = NO;
        NSError* err = nil;
        NSURL* url = [NSURL URLByResolvingBookmarkData:bookmark
                                               options:NSURLBookmarkResolutionWithoutUI
                                         relativeToURL:nil
                                   bookmarkDataIsStale:&isStale
                                                 error:&err];
        if (!url || err) return std::nullopt;

        // Refresh stale bookmark so the next persist() produces a valid token.
        if (isStale)
            persistHandle(handleFromURL(url)); // side-effect: updates bookmark

        return handleFromURL(url);
    }
};

// ─── Factory ─────────────────────────────────────────────────────────────────

bool supportsCreateFileInternal() { return false; }

std::unique_ptr<IDocumentProvider> createDocumentProvider()
{
    return std::make_unique<DocumentProviderIOS>();
}

} // namespace uapmd
