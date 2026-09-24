#import <Cocoa/Cocoa.h>
#import <SystemExtensions/SystemExtensions.h>

static NSString *const driverID = @"org.iiyume.qle2560.driver";

@interface AppDelegate : NSObject <NSApplicationDelegate, OSSystemExtensionRequestDelegate>
@property(strong) NSWindow *window;
@property(strong) NSTextField *status;
@property(strong) NSButton *activate;
@property(strong) NSButton *deactivate;
@property(strong) OSSystemExtensionRequest *pending;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 620, 250)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskMiniaturizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = @"QLE2560 Driver";
    NSTextField *intro = [NSTextField
        wrappingLabelWithString:
            @"Experimental QLE2560 Fibre Channel driver for a directly connected tape drive."];
    intro.frame = NSMakeRect(24, 155, 572, 70);
    [self.window.contentView addSubview:intro];
    self.activate = [NSButton buttonWithTitle:@"Activate Driver"
                                       target:self
                                       action:@selector(activateDriver:)];
    self.activate.frame = NSMakeRect(24, 108, 175, 32);
    [self.window.contentView addSubview:self.activate];
    self.deactivate = [NSButton buttonWithTitle:@"Deactivate Driver"
                                         target:self
                                         action:@selector(deactivateDriver:)];
    self.deactivate.frame = NSMakeRect(215, 108, 175, 32);
    [self.window.contentView addSubview:self.deactivate];
    self.status = [NSTextField
        wrappingLabelWithString:
            @"Ready. Activation requires a signed, provisioned build installed in Applications."];
    self.status.frame = NSMakeRect(24, 20, 572, 75);
    [self.window.contentView addSubview:self.status];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}
- (void)submit:(OSSystemExtensionRequest *)request {
    if (self.pending)
        return;
    self.pending = request;
    self.activate.enabled = NO;
    self.deactivate.enabled = NO;
    request.delegate = self;
    self.status.stringValue = @"Request submitted…";
    [[OSSystemExtensionManager sharedManager] submitRequest:request];
}
- (void)activateDriver:(id)sender {
    [self
        submit:[OSSystemExtensionRequest activationRequestForExtension:driverID
                                                                 queue:dispatch_get_main_queue()]];
}
- (void)deactivateDriver:(id)sender {
    [self submit:[OSSystemExtensionRequest
                     deactivationRequestForExtension:driverID
                                               queue:dispatch_get_main_queue()]];
}
- (void)requestNeedsUserApproval:(OSSystemExtensionRequest *)request {
    self.status.stringValue =
        @"macOS requires approval. Follow the system prompt in System Settings.";
}
- (OSSystemExtensionReplacementAction)request:(OSSystemExtensionRequest *)request
                  actionForReplacingExtension:(OSSystemExtensionProperties *)existing
                                withExtension:(OSSystemExtensionProperties *)extension {
    return OSSystemExtensionReplacementActionReplace;
}
- (void)finish:(NSString *)message {
    self.status.stringValue = message;
    self.pending = nil;
    self.activate.enabled = YES;
    self.deactivate.enabled = YES;
}
- (void)request:(OSSystemExtensionRequest *)request
    didFinishWithResult:(OSSystemExtensionRequestResult)result {
    [self finish:result == OSSystemExtensionRequestWillCompleteAfterReboot
                     ? @"Request will complete after reboot."
                     : @"Request completed. Use qle-tape status to check whether the tape drive is "
                       @"ready."];
}
- (void)request:(OSSystemExtensionRequest *)request didFailWithError:(NSError *)error {
    [self finish:[NSString stringWithFormat:@"%@ (%@:%ld)", error.localizedDescription,
                                            error.domain, (long)error.code]];
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        AppDelegate *delegate = [AppDelegate new];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
