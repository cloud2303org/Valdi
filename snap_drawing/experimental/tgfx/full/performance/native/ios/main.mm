#import <UIKit/UIKit.h>

#include "../render_performance.hpp"
#include <exception>

#ifndef RENDER_PERFORMANCE_EXPECTED_BACKEND
#error "The iOS shell must declare the backend of its parent-built native archive"
#endif

namespace {

NSString* text(const std::string& value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
}

NSDictionary* failure(NSString* message, id original) {
    return @{
        @"schema_version": @1,
        @"success": @NO,
        @"error": message ?: @"Unknown iOS host failure",
        @"native_report": original ?: NSNull.null,
    };
}

NSData* json(NSDictionary* report) {
    return [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
}

} // namespace

@interface RenderPerformanceDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* results;
@property(nonatomic) BOOL started;
@property(nonatomic) NSUInteger startupGeneration;
@property(atomic) BOOL foreground;
@end

@implementation RenderPerformanceDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    application.idleTimerDisabled = YES;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController* controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.systemBackgroundColor;
    self.status = [UILabel new];
    self.status.text = @"WAITING";
    self.status.font = [UIFont boldSystemFontOfSize:24];
    self.status.accessibilityIdentifier = @"render-performance-status";
    self.results = [UITextView new];
    self.results.editable = NO;
    self.results.selectable = YES;
    self.results.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.results.accessibilityIdentifier = @"render-performance-results";
    self.results.text = @"GPU render + completed readback throughput; not display FPS. Energy is unmeasured.";
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
    self.foreground = YES;
    if (self.started) { return; }
    NSUInteger generation = ++self.startupGeneration;
    self.status.text = @"SETTLING";
    // Allow launch screenshot/tree collection to finish before creating the
    // worker. The delay is outside the native core and every timing sample.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (self.started || generation != self.startupGeneration ||
            application.applicationState != UIApplicationStateActive) {
            return;
        }
        self.started = YES;
        self.status.text = @"RUNNING";
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                NSDictionary* report = nil;
                NSData* bytes = nil;
                try {
                @try {
                    if (!self.foreground) {
                        @throw [NSException exceptionWithName:@"RenderPerformanceForeground"
                                                      reason:@"App left foreground before native benchmark start"
                                                    userInfo:nil];
                    }
                    NSURL* fixtures = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"fixtures"
                                                                                      isDirectory:YES];
                    const auto native = snap::drawing::performance::runRenderPerformance(
                        fixtures.path.fileSystemRepresentation);
                    NSString* output = text(native);
                    bytes = [output dataUsingEncoding:NSUTF8StringEncoding];
                    NSError* error = nil;
                    id parsed = bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:&error] : nil;
                    if (![parsed isKindOfClass:NSDictionary.class]) {
                        report = failure(error.localizedDescription ?: @"Invalid native JSON", output);
                    } else {
                        report = parsed;
                        NSString* expectedBackend = [NSString stringWithUTF8String:RENDER_PERFORMANCE_EXPECTED_BACKEND];
                        if (![report[@"backend"] isEqualToString:expectedBackend] ||
                            ![report[@"compiler_optimized"] boolValue]) {
                            report = failure(@"Wrong backend or non-optimized native archive", report);
                        }
                    }
                } @catch (NSException* exception) {
                    report = failure(exception.description, report);
                }
                } catch (const std::exception& error) {
                    report = failure(text(error.what()), report);
                } catch (...) {
                    report = failure(@"Unknown native render benchmark failure", report);
                }
                if (!report) { report = failure(@"Native benchmark returned no report", nil); }
                bytes = json(report);
                NSError* persistenceError = nil;
                NSURL* documents = [NSFileManager.defaultManager URLForDirectory:NSDocumentDirectory
                                                                       inDomain:NSUserDomainMask
                                                              appropriateForURL:nil
                                                                         create:YES
                                                                          error:&persistenceError];
                NSURL* location = [documents URLByAppendingPathComponent:@"render-performance.json"];
                BOOL saved = bytes && location &&
                    [bytes writeToURL:location options:NSDataWritingAtomic error:&persistenceError];
                if (!saved) {
                    report = failure([@"Report persistence failed: " stringByAppendingString:
                        persistenceError.localizedDescription ?: @"unknown filesystem failure"], report);
                    bytes = json(report);
                    if (bytes && location) {
                        saved = [bytes writeToURL:location options:NSDataWritingAtomic error:nil];
                    }
                }
                BOOL passed = saved && [report[@"success"] boolValue] &&
                    [report[@"completed_scenes"] integerValue] == 5;
                NSString* output = bytes ? [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] :
                    @"{\"success\":false,\"error\":\"Cannot serialize host report\"}";
                NSString* displayed = [NSString stringWithFormat:@"%@\nReport: %@",
                    output, saved ? location.path : @"FAIL persistence; UI retains report"];
                dispatch_async(dispatch_get_main_queue(), ^{
                    self.status.text = passed ? @"PASS" : @"FAIL";
                    self.results.text = displayed;
                });
            }
        });
    });
}

- (void)applicationWillResignActive:(UIApplication*)application {
    self.foreground = NO;
    // Invalidate pending main-queue callbacks. A new active entry gets a full
    // eight seconds; an already-started native run is never duplicated.
    ++self.startupGeneration;
    if (!self.started) { self.status.text = @"WAITING"; }
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(RenderPerformanceDelegate.class));
    }
}
