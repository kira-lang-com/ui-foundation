#import <TargetConditionals.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
static NSString *KiraRunnerConfig(void) {
    NSString *path = [[NSBundle mainBundle] pathForResource:@"KiraRunner" ofType:@"toml"];
    if (path == nil) {
        return @"KiraRunner.toml not bundled";
    }
    NSError *error = nil;
    NSString *config = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    if (config == nil) {
        return [NSString stringWithFormat:@"KiraRunner.toml unreadable: %@", error.localizedDescription];
    }
    return config;
}
@interface KiraSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation KiraSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    (void)session;
    (void)connectionOptions;
    if (![scene isKindOfClass:[UIWindowScene class]]) {
        return;
    }
    NSString *config = KiraRunnerConfig();
    NSLog(@"Kira Apple runner host launched");
    NSLog(@"Kira runner config loaded: %@", config);
    UIWindowScene *windowScene = (UIWindowScene *)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
    UIViewController *controller = [UIViewController new];
    controller.view.backgroundColor = [UIColor systemBackgroundColor];
    UILabel *label = [[UILabel alloc] initWithFrame:controller.view.bounds];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.text = @"Kira runtime configured";
    [controller.view addSubview:label];
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
}
@end
@interface KiraAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation KiraAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession options:(UISceneConnectionOptions *)options {
    (void)application;
    (void)options;
    UISceneConfiguration *configuration = [[UISceneConfiguration alloc] initWithName:@"Default Configuration" sessionRole:connectingSceneSession.role];
    configuration.delegateClass = [KiraSceneDelegate class];
    return configuration;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([KiraAppDelegate class]));
    }
}
#else
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSLog(@"Kira Apple runner host launched");
        NSString *path = [[NSBundle mainBundle] pathForResource:@"KiraRunner" ofType:@"toml"];
        NSLog(@"Kira runner config path: %@", path);
    }
    return 0;
}
#endif
