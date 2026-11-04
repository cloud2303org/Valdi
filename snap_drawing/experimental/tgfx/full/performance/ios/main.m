#import <UIKit/UIKit.h>
#import <ValdiRuntime/SCValdiRuntimeManager.h>
#import <ValdiRuntime/SCValdiComponentContainerView.h>
#import <ValdiRuntime/SCValdiCustomModuleProvider.h>
#import <ValdiRuntime/SCValdiExceptionReporter.h>
#import <RenderBenchmarkTypes/RenderBenchmarkTypes.h>
#include <math.h>
#include <sys/resource.h>
#include <time.h>

// Called directly on the JS calling thread; never synchronously dispatch to main.
static double BenchmarkNowMs(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return NAN;
    return (double)value.tv_sec * 1000.0 + (double)value.tv_nsec / 1000000.0;
}
static double BenchmarkProcessCpuMs(void) {
    struct rusage value;
    if (getrusage(RUSAGE_SELF, &value) != 0) return NAN;
    return (double)value.ru_utime.tv_sec * 1000.0 + (double)value.ru_utime.tv_usec / 1000.0
         + (double)value.ru_stime.tv_sec * 1000.0 + (double)value.ru_stime.tv_usec / 1000.0;
}

@interface BenchmarkModules : NSObject <SCValdiCustomModuleProvider>
@end
@implementation BenchmarkModules
- (NSData *)customModuleDataForPath:(NSString *)modulePath error:(NSError **)error {
    NSSet *owned = [NSSet setWithArray:@[@"drawing.valdimodule", @"drawing.map.json",
                                        @"render_benchmark.valdimodule", @"render_benchmark.map.json"]];
    if (error) *error = nil;
    if (![owned containsObject:modulePath]) return nil;
    NSURL *url = [[NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"package/modules"]
                  URLByAppendingPathComponent:modulePath];
    if ([modulePath hasSuffix:@".map.json"] &&
        ![NSFileManager.defaultManager fileExistsAtPath:url.path]) return nil;
    return [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
}
@end

@interface BenchmarkDelegate : UIResponder <UIApplicationDelegate, SCValdiExceptionReporter>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) SCValdiRuntimeManager *manager;
@property(nonatomic, strong) SCValdiComponentContainerView *root;
@property(nonatomic, strong) UILabel *status;
@property(nonatomic, strong) UIStackView *stack;
@property(nonatomic, strong) NSURL *directory;
@property(nonatomic, strong) NSMutableArray *events;
@property(nonatomic, strong) NSDictionary *provenance;
@property(nonatomic, copy) NSString *backend;
@property(nonatomic, strong) NSNumber *startedMs;
@property(nonatomic, strong) NSNumber *startedCpuMs;
@property(nonatomic, strong) NSDictionary *cpuProxy;
@property(nonatomic, strong) NSMutableArray<NSString *> *sceneIds;
@property(nonatomic) BOOL sceneFailed;
@property(nonatomic) BOOL terminal;
@property(nonatomic) BOOL destroyed;
@property(nonatomic) BOOL startupReady;
@property(nonatomic) BOOL runtimeStarted;
@property(nonatomic, copy) dispatch_block_t delayedStartup;
@end

@implementation BenchmarkDelegate
- (NSArray<NSString *> *)suiteIds {
    return @[@"cached-vector", @"shaped-text", @"image-svg", @"lottie"];
}
- (void)require:(BOOL)value message:(NSString *)message {
    if (!value) [NSException raise:@"BenchmarkContract" format:@"%@", message];
}
- (void)writeData:(NSData *)data to:(NSURL *)url {
    NSError *error = nil;
    [self require:data != nil && url != nil message:@"Missing persistence data/path"];
    if (![data writeToURL:url options:NSDataWritingAtomic error:&error])
        [NSException raise:@"BenchmarkPersistence" format:@"%@", error];
}
- (void)persist:(NSString *)value {
    NSMutableDictionary *record = [@{
        @"schema": @1, @"backend": self.backend ?: @"unknown", @"status": value,
        @"runId": self.directory.lastPathComponent ?: @"unavailable",
        @"completed": @(self.terminal), @"sceneCount": @(self.sceneIds.count),
        @"startupSettleMs": @8000,
        @"rootMode": @"native UIKit",
        @"measurement": @"Managed render + synchronous readback throughput/GPU frame latency; not onscreen FPS",
        @"events": self.events ?: @[], @"provenance": self.provenance ?: @{},
        @"cpuProxy": self.cpuProxy ?: (id)NSNull.null,
        @"directory": self.directory.path ?: @"unavailable",
        @"updatedUnixMs": @(NSDate.date.timeIntervalSince1970 * 1000.0)
    } mutableCopy];
    NSString *line = [NSString stringWithFormat:@"%@ %@", self.backend ?: @"unknown", value];
    @try {
        [self require:self.directory != nil message:@"No report directory"];
        NSError *error = nil;
        NSData *data = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:&error];
        [self require:data != nil message:error.description];
        [self writeData:data to:[self.directory URLByAppendingPathComponent:@"results.json"]];
        // The terminal marker commits results.json; never write it first.
        if (self.terminal)
            [self writeData:[[line stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]
                         to:[self.directory URLByAppendingPathComponent:@"done"]];
        self.status.text = [NSString stringWithFormat:@"%@ scenes=%lu\n%@\nResult: results.json; terminal: done",
                            line, (unsigned long)self.sceneIds.count, self.directory.path];
    } @catch (NSException *exception) {
        self.terminal = YES;
        [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(watchdog) object:nil];
        record[@"status"] = @"FAIL";
        record[@"completed"] = @YES;
        record[@"persistenceError"] = exception.description;
        if (self.directory) {
            [NSFileManager.defaultManager removeItemAtURL:[self.directory URLByAppendingPathComponent:@"done"] error:nil];
            @try {
                NSData *data = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:nil];
                [self writeData:data to:[self.directory URLByAppendingPathComponent:@"results.json"]];
                [self writeData:[[NSString stringWithFormat:@"%@ FAIL\n", self.backend ?: @"unknown"]
                                     dataUsingEncoding:NSUTF8StringEncoding]
                             to:[self.directory URLByAppendingPathComponent:@"done"]];
            } @catch (NSException *ignored) { /* UI cannot claim persisted success. */ }
        }
        self.status.text = [NSString stringWithFormat:@"%@ FAIL report persistence\n%@",
                            self.backend ?: @"unknown", self.directory.path ?: @"unavailable"];
    }
}
- (void)fail:(NSString *)message {
    if (self.terminal) return;
    self.terminal = YES;
    [self cancelStartup];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(watchdog) object:nil];
    [self.events addObject:@{@"nativeFailure": @{@"message": message ?: @"Unknown native failure"}}];
    [self persist:@"FAIL"];
}
- (void)watchdog { [self fail:@"No finished report within 180 seconds of native startup"]; }
- (void)checkProvenance:(NSDictionary *)declared {
    [self require:[declared isKindOfClass:NSDictionary.class] message:@"Missing JS provenance"];
    // Shared compile inputs carry no backend authority. Only the external stage
    // identifies the runtime artifact used by this native host.
    [self require:[declared[@"module"] isEqual:@"render_benchmark"] &&
                  [declared[@"compilation"] isEqual:@"release"] &&
                  [declared[@"runtimeBinding"] isEqual:@"unbound"] message:@"Unexpected module provenance"];
    for (NSString *key in @[@"backend", @"runtimeSha256", @"config"]) {
        if (declared[key] && declared[key] != NSNull.null)
            [self require:[declared[key] isEqual:self.provenance[key]]
                   message:[@"JS/native mismatch: " stringByAppendingString:key]];
    }
}
- (void)receive:(NSString *)text monotonicMs:(double)receiptMs cpuMs:(double)receiptCpuMs {
    if (self.terminal) return;
    @try {
        NSError *error = nil;
        id parsed = [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding]
                                                  options:0 error:&error];
        [self require:[parsed isKindOfClass:NSDictionary.class] message:error.description ?: @"Report is not an object"];
        NSDictionary *event = parsed;
        [self require:[event[@"schema"] isEqual:@1] message:@"Unknown event schema"];
        [self require:isfinite(receiptMs) && isfinite(receiptCpuMs) message:@"Native clock/CPU sample failed"];
        [self.events addObject:@{@"event": event, @"nativeReceipt":
            @{@"monotonicMs": @(receiptMs), @"processCpuMs": @(receiptCpuMs)}}];
        NSString *kind = event[@"kind"];
        if ([kind isEqual:@"started"]) {
            [self require:self.startedMs == nil message:@"Duplicate started"];
            [self require:[event[@"suiteIds"] isEqual:self.suiteIds] message:@"Unexpected benchmark suite"];
            [self checkProvenance:event[@"provenance"]];
            self.startedMs = @(receiptMs);
            self.startedCpuMs = @(receiptCpuMs);
        } else if ([kind isEqual:@"scene"]) {
            [self require:self.startedMs != nil message:@"Scene before started"];
            NSDictionary *result = event[@"result"];
            NSUInteger index = self.sceneIds.count;
            [self require:[result isKindOfClass:NSDictionary.class] && index < self.suiteIds.count
                          && [result[@"id"] isEqual:self.suiteIds[index]]
                          && [event[@"index"] isEqual:@(index)] message:@"Missing/reordered/duplicate scene"];
            [self.sceneIds addObject:result[@"id"]];
            if (![event[@"status"] isEqual:@"PASS"] || ![result[@"completed"] isEqual:@YES]
                || ![result[@"status"] isEqual:@"PASS"]) self.sceneFailed = YES;
            // Keep failed scene payloads and await the module's complete finish.
        } else if ([kind isEqual:@"finished"]) {
            [self require:self.startedMs != nil message:@"Finished before started"];
            double elapsed = receiptMs - self.startedMs.doubleValue;
            double cpu = receiptCpuMs - self.startedCpuMs.doubleValue;
            [self require:elapsed > 0 && cpu >= 0 message:@"Invalid native timing samples"];
            self.cpuProxy = @{
                @"method": @"getrusage(RUSAGE_SELF): user + system",
                @"scope": @"Whole process, native started/finished callback entry",
                @"startedProcessCpuMs": self.startedCpuMs, @"finishedProcessCpuMs": @(receiptCpuMs),
                @"processCpuMs": @(cpu), @"elapsedMonotonicMs": @(elapsed),
                @"processCpuMsPerWallMs": @(cpu / elapsed),
                @"interpretation": @"CPU activity proxy; ratio may exceed 1; not watts or energy"
            };
            [self require:[event[@"status"] isEqual:@"PASS"] && [event[@"completed"] isEqual:@YES]
                          && [self.sceneIds isEqualToArray:self.suiteIds]
                          && !self.sceneFailed
                          && [event[@"suiteIds"] isEqual:self.suiteIds] message:@"Incomplete/failed benchmark"];
            [self checkProvenance:event[@"provenance"]];
            NSDictionary *engine = event[@"engine"];
            [self require:[engine isKindOfClass:NSDictionary.class] && [engine[@"verified"] isEqual:@YES]
                          && [engine[@"name"] isEqual:@"Hermes"] message:@"Hermes not verified"];
            NSArray *scenes = event[@"scenes"];
            [self require:[scenes isKindOfClass:NSArray.class] && scenes.count == self.suiteIds.count
                   message:@"Incomplete final scene payload"];
            for (NSUInteger i = 0; i < scenes.count; i++) {
                NSDictionary *scene = scenes[i];
                [self require:[scene isKindOfClass:NSDictionary.class] &&
                              [scene[@"id"] isEqual:self.suiteIds[i]] &&
                              [scene[@"completed"] isEqual:@YES] && [scene[@"status"] isEqual:@"PASS"]
                       message:@"Final scene failed"];
            }
            self.terminal = YES;
            [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(watchdog) object:nil];
            [self persist:@"PASS"];
            return;
        } else {
            [NSException raise:@"BenchmarkContract" format:@"Unknown event kind %@", kind];
        }
        [self persist:@"RUNNING"];
    } @catch (NSException *exception) {
        [self fail:[NSString stringWithFormat:@"Report rejected: %@\n%@", exception, text]];
    }
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.events = [NSMutableArray new];
    self.sceneIds = [NSMutableArray new];
    self.backend = @"unknown";
    self.status = [UILabel new];
    self.status.numberOfLines = 0;
    self.status.text = @"unknown STARTING";
    self.status.accessibilityIdentifier = @"render-benchmark-status";
    UIViewController *controller = [UIViewController new];
    controller.view.backgroundColor = UIColor.whiteColor;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[self.status]];
    self.stack = stack;
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
    application.idleTimerDisabled = YES;
    @try {
        NSError *error = nil;
        NSURL *documents = [NSFileManager.defaultManager URLForDirectory:NSDocumentDirectory
            inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:&error];
        [self require:documents != nil message:error.description ?: @"Documents unavailable"];
        NSURL *base = [documents URLByAppendingPathComponent:@"render-benchmark" isDirectory:YES];
        self.directory = [base URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
        [self require:[NSFileManager.defaultManager createDirectoryAtURL:self.directory
            withIntermediateDirectories:YES attributes:nil error:&error] message:error.description];
        [self writeData:[self.directory.path dataUsingEncoding:NSUTF8StringEncoding]
                     to:[base URLByAppendingPathComponent:@"latest.txt"]];
        NSURL *url = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"provenance.json"];
        NSData *data = [NSData dataWithContentsOfURL:url options:0 error:&error];
        [self require:data != nil message:error.description ?: @"Missing stage provenance"];
        id provenance = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
        [self require:[provenance isKindOfClass:NSDictionary.class] message:@"Invalid stage provenance"];
        self.provenance = provenance;
        self.backend = self.provenance[@"backend"];
        [self require:[@[@"skia", @"tgfx"] containsObject:self.backend] message:@"Invalid backend"];
        [self require:[self.provenance[@"config"] isKindOfClass:NSString.class] &&
                      [self.provenance[@"config"] caseInsensitiveCompare:@"Release"] == NSOrderedSame
               message:@"Stage is not Release"];
        NSString *sha = self.provenance[@"runtimeSha256"];
        [self require:[sha isKindOfClass:NSString.class] && sha.length == 64 &&
                      [sha rangeOfCharacterFromSet:
                          [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location == NSNotFound
               message:@"Missing runtime hash"];
        [self persist:@"SETTLING"];
        if (self.terminal) return YES;
        [self performSelector:@selector(watchdog) withObject:nil afterDelay:180];
        self.startupReady = YES;
    } @catch (NSException *exception) {
        [self fail:[NSString stringWithFormat:@"Native startup: %@\n%@", exception, exception.callStackSymbols]];
    }
    return YES;
}
- (void)cancelStartup {
    if (self.delayedStartup) dispatch_block_cancel(self.delayedStartup);
    self.delayedStartup = nil;
}
- (void)applicationDidBecomeActive:(UIApplication *)application {
    if (!self.startupReady || self.runtimeStarted || self.terminal || self.destroyed || self.delayedStartup) return;
    __weak BenchmarkDelegate *weakSelf = self;
    dispatch_block_t startup = dispatch_block_create(0, ^{
        BenchmarkDelegate *host = weakSelf;
        if (!host || host.destroyed || host.terminal || host.runtimeStarted ||
            UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
        host.delayedStartup = nil;
        host.runtimeStarted = YES;
        [host startRuntime];
    });
    self.delayedStartup = startup;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), startup);
}
- (void)applicationWillResignActive:(UIApplication *)application {
    [self cancelStartup];
}
- (void)startRuntime {
    @try {
        self.manager = [SCValdiRuntimeManager new];
        [self.manager updateConfiguration:^(SCValdiConfiguration *configuration) {
            configuration.customModuleProvider = [BenchmarkModules new];
            configuration.javaScriptEngineType = SCNValdiCoreJavaScriptEngineTypeHermes;
            configuration.enableDebuggerService = NO;
            configuration.disableHotReloader = YES;
            configuration.exceptionReporter = self;
        }];
        __weak BenchmarkDelegate *weakSelf = self;
        // Generated block and initializer types; inspect emitted headers before
        // the parent's native build. No handwritten exported ABI declarations.
        RenderBenchmarkBenchmarkContextReportBlock report = ^(NSString *text) {
            double receiptMs = BenchmarkNowMs();
            double receiptCpuMs = BenchmarkProcessCpuMs();
            dispatch_async(dispatch_get_main_queue(), ^{
                [weakSelf receive:text monotonicMs:receiptMs cpuMs:receiptCpuMs];
            });
        };
        RenderBenchmarkBenchmarkContextNowMsBlock nowMs = ^double {
            return BenchmarkNowMs();
        };
        RenderBenchmarkBenchmarkContext *context = [[RenderBenchmarkBenchmarkContext alloc]
            initWithReport:report nowMs:nowMs];
        self.root = [[SCValdiComponentContainerView alloc]
            initWithComponentPath:@"BenchmarkApp@render_benchmark/src/Benchmark"
            owner:nil viewModel:nil componentContext:context runtime:self.manager.mainRuntime];
        self.root.accessibilityIdentifier = @"render-benchmark-root";
        [self.stack addArrangedSubview:self.root];
    } @catch (NSException *exception) {
        [self fail:[NSString stringWithFormat:@"Native startup: %@\n%@", exception, exception.callStackSymbols]];
    }
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
    self.destroyed = YES;
    [self cancelStartup];
    if (!self.terminal) [self fail:@"Application terminated before benchmark finished"];
    [self.root.valdiContext destroy];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(BenchmarkDelegate.class));
    }
}
