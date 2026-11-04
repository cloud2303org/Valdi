#import <UIKit/UIKit.h>
#import <ValdiRuntime/SCValdiRuntimeManager.h>
#import <ValdiRuntime/SCValdiComponentContainerView.h>
#import <ValdiRuntime/SCValdiCustomModuleProvider.h>
#import <ValdiRuntime/SCValdiExceptionReporter.h>
#import <TGFXAcceptanceTypes/TGFXAcceptanceTypes.h>

@interface AcceptanceModules : NSObject <SCValdiCustomModuleProvider>
@end
@implementation AcceptanceModules
- (NSData *)customModuleDataForPath:(NSString *)modulePath error:(NSError **)error {
    NSSet *owned = [NSSet setWithArray:@[@"tgfx_acceptance.valdimodule", @"tgfx_acceptance.map.json",
                                        @"drawing.valdimodule", @"drawing.map.json"]];
    if (![owned containsObject:modulePath]) {
        if (error) *error = nil;
        return nil;
    }
    NSURL *url = [[NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"package/modules"]
                    URLByAppendingPathComponent:modulePath];
    return [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
}
@end

@interface AcceptanceDelegate : UIResponder <UIApplicationDelegate, SCValdiExceptionReporter>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) SCValdiRuntimeManager *manager;
@property(nonatomic, strong) SCValdiComponentContainerView *root;
@property(nonatomic, strong) UILabel *status;
@property(nonatomic, strong) UITextView *results;
@property(nonatomic, strong) NSURL *directory;
@property(nonatomic, strong) NSMutableArray *events;
@property(nonatomic, strong) NSMutableArray<NSString *> *captured;
@property(nonatomic, strong) NSDictionary *provenance;
@property(nonatomic) BOOL failed;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL started;
@end

@implementation AcceptanceDelegate
- (NSArray<NSString *> *)caseIds {
    return @[@"composition", @"shape", @"label-iiii", @"label-mmmm",
             @"bitmap-bytes", @"bitmap-catalog", @"svg", @"lottie-t0", @"lottie-t1"];
}
- (void)require:(BOOL)condition message:(NSString *)message {
    if (!condition) [NSException raise:@"AcceptanceContract" format:@"%@", message];
}
- (void)writeData:(NSData *)data to:(NSURL *)url {
    NSError *error = nil;
    if (![data writeToURL:url options:NSDataWritingAtomic error:&error])
        [NSException raise:@"AcceptancePersistence" format:@"%@", error];
}
- (void)writeState:(NSString *)value {
    NSDictionary *record = @{
        @"schema": @1, @"status": value, @"completed": @(self.finished),
        @"rootMode": @"native",
        @"gate": @"Hermes application + managed TGFX output; not iOS screen compositor acceptance",
        @"events": self.events ?: @[], @"directory": self.directory.path ?: @"unavailable",
        @"provenance": self.provenance ?: @{},
        @"completedUnixMs": @(NSDate.date.timeIntervalSince1970 * 1000)
    };
    @try {
        NSError *error = nil;
        NSData *data = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:&error];
        [self require:data != nil && self.directory != nil message:error.description ?: @"No report directory"];
        [self writeData:data to:[self.directory URLByAppendingPathComponent:@"results.json"]];
        if (self.finished) [self writeData:[value dataUsingEncoding:NSUTF8StringEncoding]
                                       to:[self.directory URLByAppendingPathComponent:@"done"]];
        self.status.text = [NSString stringWithFormat:@"%@ %lu/%lu root=native", value,
                            (unsigned long)self.captured.count, (unsigned long)self.caseIds.count];
        self.results.text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    } @catch (NSException *exception) {
        self.failed = YES;
        self.finished = YES;
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(watchdog) object:nil];
        BOOL savedFailure = NO;
        NSMutableDictionary *failure = [record mutableCopy];
        failure[@"status"] = @"FAIL";
        failure[@"completed"] = @YES;
        failure[@"persistenceError"] = exception.description;
        if (self.directory) {
            [NSFileManager.defaultManager removeItemAtURL:[self.directory URLByAppendingPathComponent:@"done"] error:nil];
            @try {
                NSData *data = [NSJSONSerialization dataWithJSONObject:failure options:NSJSONWritingPrettyPrinted error:nil];
                [self writeData:data to:[self.directory URLByAppendingPathComponent:@"results.json"]];
                [self writeData:[@"FAIL" dataUsingEncoding:NSUTF8StringEncoding]
                             to:[self.directory URLByAppendingPathComponent:@"done"]];
                savedFailure = YES;
            } @catch (NSException *ignored) { /* UI stays FAIL even if storage is unavailable. */ }
        }
        self.status.text = @"FAIL report persistence";
        self.results.text = [NSString stringWithFormat:@"%@\n%@\n%@", failure,
                             savedFailure ? @"Failure report saved" : @"REPORT NOT SAVED", exception.callStackSymbols];
    }
}
- (void)fail:(NSString *)message {
    self.failed = YES;
    self.finished = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(watchdog) object:nil];
    [self.events addObject:@{@"kind": @"nativeFailure", @"message": message ?: @"Unknown failure"}];
    [self writeState:@"FAIL"];
}
- (void)watchdog { [self fail:@"Host watchdog: no completed JS acceptance within 180 seconds"]; }
- (void)receive:(NSString *)text {
    if (self.finished) return;
    @try {
        NSError *error = nil;
        NSMutableDictionary *event = [[NSJSONSerialization JSONObjectWithData:
            [text dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingMutableContainers error:&error] mutableCopy];
        [self require:[event isKindOfClass:NSDictionary.class] && [event[@"schema"] isEqual:@1]
               message:error.description ?: @"Invalid report schema"];
        NSString *kind = event[@"kind"];
        if ([kind isEqual:@"started"]) {
            [self require:!self.started message:@"Duplicate started event"];
            [self require:[event[@"engine"][@"verified"] isEqual:@YES] &&
                          [event[@"engine"][@"name"] isEqual:@"Hermes"] message:@"Hermes not verified"];
            for (NSString *key in @[@"runtimeSha256", @"sourceStateSha256", @"sourceRoot"])
                [self require:[event[@"provenance"][key] isEqual:self.provenance[key]]
                       message:@"JS/native provenance mismatch"];
            self.started = YES;
        } else if ([kind isEqual:@"case"]) {
            [self require:self.started message:@"Case before engine verification"];
            NSDictionary *result = event[@"result"];
            NSString *caseId = result[@"id"];
            [self require:[self.caseIds containsObject:caseId] && ![self.captured containsObject:caseId]
                   message:@"Unknown/duplicate case"];
            [self require:[result[@"assertions"] count] > 0 message:@"No content assertions"];
            NSData *png = [[NSData alloc] initWithBase64EncodedString:event[@"pngBase64"] options:0];
            UIImage *image = [UIImage imageWithData:png];
            [self require:image.CGImage != nil && CGImageGetWidth(image.CGImage) == 160 &&
                          CGImageGetHeight(image.CGImage) == 120 message:@"Invalid PNG dimensions"];
            NSString *name = [caseId stringByAppendingString:@".png"];
            [self writeData:png to:[self.directory URLByAppendingPathComponent:name]];
            [self.captured addObject:caseId];
            [event removeObjectForKey:@"pngBase64"];
            event[@"pngFile"] = name;
            if (![result[@"passed"] isEqual:@YES]) self.failed = YES;
        } else if ([kind isEqual:@"finished"]) {
            NSArray *cases = event[@"cases"];
            [self require:self.started && [self.captured isEqualToArray:self.caseIds] &&
                          cases.count == self.caseIds.count message:@"Incomplete case set"];
            for (NSUInteger i = 0; i < cases.count; i++) {
                NSDictionary *item = cases[i];
                [self require:[item[@"id"] isEqual:self.caseIds[i]] && [item[@"passed"] isEqual:@YES]
                       message:@"Failed/reordered case"];
                [self require:[item[@"assets"][@"errors"] count] == 0 message:@"Asset errors"];
                NSArray *assertions = item[@"assertions"];
                [self require:assertions.count > 0 message:@"Missing assertions"];
                for (NSDictionary *assertion in assertions)
                    [self require:[assertion[@"passed"] isEqual:@YES] message:@"Failed content assertion"];
            }
            [self require:[event[@"status"] isEqual:@"PASS"] && !self.failed message:@"JS/native failure"];
            [self.events addObject:event];
            self.finished = YES;
            [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(watchdog) object:nil];
            [self writeState:@"PASS"];
            return;
        } else {
            [NSException raise:@"AcceptanceContract" format:@"Unknown event kind %@", kind];
        }
        [self.events addObject:event];
        [self writeState:self.failed ? @"FAIL" : @"RUNNING"];
    } @catch (NSException *exception) {
        [self fail:[NSString stringWithFormat:@"Report rejected: %@\n%@\n%@", exception, exception.callStackSymbols, text]];
    }
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.events = [NSMutableArray new];
    self.captured = [NSMutableArray new];
    self.status = [UILabel new];
    self.status.numberOfLines = 0;
    self.status.text = @"STARTING";
    self.status.accessibilityIdentifier = @"tgfx-app-status";
    self.results = [UITextView new];
    self.results.editable = NO;
    self.results.accessibilityIdentifier = @"tgfx-app-results";
    UIViewController *controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.whiteColor;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.status, self.results]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [controller.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:controller.view.safeAreaLayoutGuide.trailingAnchor]
    ]];
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
    @try {
        NSURL *documents = [NSFileManager.defaultManager URLForDirectory:NSDocumentDirectory
            inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
        NSURL *base = [documents URLByAppendingPathComponent:@"tgfx-app" isDirectory:YES];
        self.directory = [base URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        NSError *error = nil;
        [self require:[NSFileManager.defaultManager createDirectoryAtURL:self.directory
                withIntermediateDirectories:YES attributes:nil error:&error] message:error.description];
        [self writeData:[self.directory.path dataUsingEncoding:NSUTF8StringEncoding]
                     to:[base URLByAppendingPathComponent:@"latest.txt"]];
        NSURL *provenanceURL = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"package/provenance.json"];
        NSData *provenanceData = [NSData dataWithContentsOfURL:provenanceURL options:0 error:&error];
        [self require:provenanceData != nil message:error.description ?: @"Missing native provenance"];
        self.provenance = [NSJSONSerialization JSONObjectWithData:provenanceData options:0 error:&error];
        [self require:[self.provenance[@"schema"] isEqual:@2] &&
                      [self.provenance[@"kind"] isEqual:@"tgfx-valdi-runtime-export"] &&
                      [self.provenance[@"runtimeBinding"] isEqual:@"exported"] &&
                      [self.provenance[@"nativeAcceptance"] isEqual:@NO] &&
                      [self.provenance[@"profile"] isEqual:@"ios_sim_arm64"] &&
                      [self.provenance[@"backend"] isEqual:@"tgfx"] &&
                      [self.provenance[@"engine"] isEqual:@"hermes"] message:@"Invalid native provenance"];
        [self writeState:@"STARTING"];
        if (self.finished) return YES;
        [self performSelector:@selector(watchdog) withObject:nil afterDelay:180];
        self.manager = [SCValdiRuntimeManager new];
        [self.manager updateConfiguration:^(SCValdiConfiguration *configuration) {
            configuration.customModuleProvider = [AcceptanceModules new];
            configuration.javaScriptEngineType = SCNValdiCoreJavaScriptEngineTypeHermes;
            configuration.enableDebuggerService = NO;
            configuration.disableHotReloader = YES;
            configuration.exceptionReporter = self;
        }];
        __weak AcceptanceDelegate *weakSelf = self;
        // The actual node-diagnostics-01 generated header supplies this typedef
        // and initWithReport:. No handwritten callback bridge declaration.
        TGFXAcceptanceAcceptanceContextReportBlock report = ^(NSString *event) {
            dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf receive:event]; });
        };
        TGFXAcceptanceAcceptanceContext *context = [[TGFXAcceptanceAcceptanceContext alloc] initWithReport:report];
        self.root = [[SCValdiComponentContainerView alloc]
            initWithComponentPath:@"AcceptanceApp@tgfx_acceptance/src/Acceptance"
            owner:nil viewModel:nil componentContext:context runtime:self.manager.mainRuntime];
        self.root.accessibilityIdentifier = @"tgfx-app-preview";
        [self.root.heightAnchor constraintEqualToConstant:180].active = YES;
        [stack insertArrangedSubview:self.root atIndex:1];
    } @catch (NSException *exception) {
        [self fail:[NSString stringWithFormat:@"Native startup: %@\n%@", exception, exception.callStackSymbols]];
    }
    return YES;
}
- (void)reportNonFatalWithErrorCode:(NSInteger)errorCode message:(NSString *)message
                            module:(NSString *)module stackTrace:(NSString *)stackTrace {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self fail:[NSString stringWithFormat:@"Valdi %ld %@: %@\n%@", (long)errorCode, module, message, stackTrace]];
    });
}
- (void)reportCrashWithMessage:(NSString *)message module:(NSString *)module
                   stackTrace:(NSString *)stackTrace isANR:(BOOL)isANR {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self fail:[NSString stringWithFormat:@"Valdi crash/ANR=%d %@: %@\n%@", isANR, module, message, stackTrace]];
    });
}
- (void)applicationWillTerminate:(UIApplication *)application {
    if (!self.finished) [self fail:@"Application terminated before acceptance completed"];
    [self.root.valdiContext destroy];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AcceptanceDelegate.class));
    }
}
