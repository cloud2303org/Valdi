#import <UIKit/UIKit.h>
#include "vector_checks.hpp"

@interface ProbeDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic, strong) UILabel* status;
@property(nonatomic, strong) UITextView* results;
@property(nonatomic) BOOL started;
@end

@implementation ProbeDelegate
- (BOOL)application:(UIApplication*)application
        didFinishLaunchingWithOptions:(NSDictionary*)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController* controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.whiteColor;
    self.status = [UILabel new];
    self.status.font = [UIFont boldSystemFontOfSize:26];
    self.status.text = @"Running real TGFX checks";
    self.status.accessibilityIdentifier = @"tgfx-status";
    self.results = [UITextView new];
    self.results.editable = NO;
    self.results.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular];
    self.results.accessibilityIdentifier = @"tgfx-result";
    self.results.text = @"SnapDrawing vector sources + TGFX / OpenGL ES";
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.status, self.results]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 18;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [controller.view addSubview:stack];
    UILayoutGuide* safe = controller.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-20],
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
    // Let TGFX's real foreground notification open its EAGL lifecycle gate.
    dispatch_async(dispatch_get_main_queue(), ^{
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            @autoreleasepool {
                auto report = runVectorChecks();
                NSString* text = [NSString stringWithUTF8String:report.details.c_str()];
                BOOL success = report.success && report.passed == 16;
                dispatch_async(dispatch_get_main_queue(), ^{
                    self.status.text = success ? @"PASS 16/16" : @"FAIL";
                    self.status.textColor = success ? UIColor.systemGreenColor : UIColor.systemRedColor;
                    self.results.text = text;
                    NSLog(@"TGFX_VECTOR_RESULT %@", text);
                });
            }
        });
    });
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(ProbeDelegate.class));
    }
}
