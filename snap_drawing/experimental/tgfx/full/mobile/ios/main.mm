#import <UIKit/UIKit.h>

#include "full_checks.hpp"

#include <exception>
#include <string>

namespace {

NSString* reportString(const std::string& value) {
    NSString* text = [[NSString alloc] initWithBytes:value.data()
                                            length:value.size()
                                          encoding:NSUTF8StringEncoding];
    return text ?: @"FAIL: native report is not valid UTF-8";
}

BOOL persistReport(const tgfx_full_mobile::FullChecksReport& report, NSURL** location, NSString** failure) {
    @try {
        NSError* error = nil;
        NSURL* documents = [NSFileManager.defaultManager URLForDirectory:NSDocumentDirectory
                                                               inDomain:NSUserDomainMask
                                                      appropriateForURL:nil
                                                                 create:YES
                                                                  error:&error];
        if (documents == nil) {
            *failure = error.localizedDescription ?: @"Cannot locate app Documents directory";
            return NO;
        }
        *location = [documents URLByAppendingPathComponent:@"full-results.txt" isDirectory:NO];
        NSString* header = [NSString stringWithFormat:
            @"status=%@\nsuccess=%@\npassed=%d\nfailed=%d\nregistered=%d\nrunnerFailures=%d\n"
             @"completedUnixMs=%.0f\n\n--- report.details (verbatim) ---\n",
            reportString(report.status), report.success ? @"true" : @"false",
            report.passed, report.failed, report.registered, report.runnerFailures,
            NSDate.date.timeIntervalSince1970 * 1000.0];
        NSMutableData* contents = [[header dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
        // Write the actual bytes, including any non-UTF-8 diagnostics or NULs.
        [contents appendBytes:report.details.data() length:report.details.size()];
        if (![contents writeToURL:*location options:NSDataWritingAtomic error:&error]) {
            *failure = error.localizedDescription ?: @"Cannot write full-results.txt";
            return NO;
        }
        return YES;
    } @catch (NSException* exception) {
        *failure = [NSString stringWithFormat:@"%@\n%@", exception.description,
            [exception.callStackSymbols componentsJoinedByString:@"\n"]];
        return NO;
    }
}

// Copy the packaged directory itself, preserving filenames (including malformed
// media intentionally used as negative fixtures). Native checks consume files.
NSURL* copyFixtures(NSError** error) {
    NSFileManager* files = NSFileManager.defaultManager;
    NSURL* source = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"fixtures" isDirectory:YES];
    NSURL* cache = [files URLForDirectory:NSCachesDirectory
                               inDomain:NSUserDomainMask
                      appropriateForURL:nil
                                 create:YES
                                  error:error];
    if (cache == nil) {
        return nil;
    }
    NSURL* destination = [cache URLByAppendingPathComponent:
        [@"tgfx-full-fixtures-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
    if (![files copyItemAtURL:source toURL:destination error:error]) {
        return nil;
    }
    return destination;
}

} // namespace

@interface FullProbeDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* results;
@property(nonatomic) BOOL started;
@end

@implementation FullProbeDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController* controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
    self.status = [UILabel new];
    self.status.numberOfLines = 0;
    self.status.font = [UIFont boldSystemFontOfSize:24];
    self.status.text = @"WAITING for foreground";
    self.status.accessibilityIdentifier = @"tgfx-full-status";
    self.results = [UITextView new];
    self.results.editable = NO;
    self.results.selectable = YES;
    self.results.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.results.accessibilityIdentifier = @"tgfx-full-results";
    self.results.text = @"Real full backend GPU effects/media/text checks; all registered cases must pass.";
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.status, self.results]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [controller.view addSubview:stack];
    UILayoutGuide* safe = controller.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:16],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16],
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],
    ]];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    return YES;
}

- (void)applicationDidBecomeActive:(UIApplication*)application {
    if (self.started) {
        return;
    }
    self.started = YES;
    self.status.text = @"RUNNING";
    // Give TGFX's real foreground observers this main-loop turn before creating
    // an offscreen device on the worker. Only one run starts per app process.
    dispatch_async(dispatch_get_main_queue(), ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                tgfx_full_mobile::FullChecksReport report;
                NSURL* fixtures = nil;
                try {
                    @try {
                        NSError* error = nil;
                        fixtures = copyFixtures(&error);
                        if (fixtures == nil) {
                            report = tgfx_full_mobile::startupFailure(
                                std::string("Cannot copy bundled fixtures: ") +
                                (error.localizedDescription.UTF8String ?: "unknown filesystem error"));
                        } else {
                            NSURL* media = [fixtures URLByAppendingPathComponent:@"media-data" isDirectory:YES];
                            NSURL* text = [fixtures URLByAppendingPathComponent:@"text-data" isDirectory:YES];
                            report = tgfx_full_mobile::runFullChecks(media.path.fileSystemRepresentation,
                                                                    text.path.fileSystemRepresentation);
                        }
                        if (fixtures != nil) {
                            [NSFileManager.defaultManager removeItemAtURL:fixtures error:nil];
                        }
                    } @catch (NSException* exception) {
                        NSString* details = [NSString stringWithFormat:@"%@\n%@", exception.description,
                            [exception.callStackSymbols componentsJoinedByString:@"\n"]];
                        if (report.details.empty()) {
                            report = tgfx_full_mobile::startupFailure(details.UTF8String);
                        } else {
                            // Retain a completed native report if cleanup raises.
                            tgfx_full_mobile::recordRunnerFailure(report, details.UTF8String);
                        }
                    }
                } catch (const std::exception& failure) {
                    report = tgfx_full_mobile::startupFailure(failure.what());
                } catch (...) {
                    report = tgfx_full_mobile::startupFailure("Unknown native app-shell exception");
                }

                NSURL* reportFile = nil;
                NSString* persistenceFailure = nil;
                BOOL saved = persistReport(report, &reportFile, &persistenceFailure);
                if (!saved) {
                    tgfx_full_mobile::recordRunnerFailure(report,
                        std::string("Cannot persist full-results.txt: ") +
                        (persistenceFailure.UTF8String ?: "unknown filesystem error"));
                    report.status = "FAIL report persistence";
                    report.details = "FAIL report persistence; original report follows\n" + report.details;
                    // One bounded attempt to save the explicit failure, retaining
                    // every original diagnostic and count. It cannot restore PASS.
                    saved = persistReport(report, &reportFile, &persistenceFailure);
                    if (!saved) {
                        tgfx_full_mobile::recordRunnerFailure(report,
                            std::string("Failure-report persistence also failed: ") +
                            (persistenceFailure.UTF8String ?: "unknown filesystem error"));
                        report.status = "FAIL report persistence";
                    }
                }
                NSString* status = reportString(report.status);
                NSString* location = [NSString stringWithFormat:@"%@: %@",
                    saved ? @"Saved report" : @"REPORT NOT SAVED",
                    reportFile.path ?: @"Documents/full-results.txt (directory unavailable)"];
                NSString* details = [NSString stringWithFormat:@"%@\n\n%@", reportString(report.details), location];
                const BOOL success = report.success;
                // Persistence (including startup failures) completes before the
                // final UI dispatch. Logs carry only a short retrieval pointer.
                NSLog(@"TGFX_FULL_RESULT %@ passed=%d failed=%d registered=%d runnerFailures=%d %@",
                    status, report.passed, report.failed, report.registered, report.runnerFailures, location);
                dispatch_async(dispatch_get_main_queue(), ^{
                    self.status.text = status;
                    self.status.textColor = success ? UIColor.systemGreenColor : UIColor.systemRedColor;
                    self.results.text = details;
                });
            }
        });
    });
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(FullProbeDelegate.class));
    }
}
