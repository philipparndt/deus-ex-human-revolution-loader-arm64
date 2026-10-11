// ObjC bridge test (legacy i386 runtime): host classes, guest subclasses,
// super calls, properties, exceptions, fast enumeration, notifications.
#import <Foundation/Foundation.h>

static int fails;
#define CHECK(c) do { if (c) printf("PASS %s\n", #c); else { printf("FAIL %s\n", #c); fails++; } } while (0)

@interface Agent : NSObject {
    int _level;
    NSString *_name;
    int _notified;
}
@property (copy) NSString *name;
- (id)initWithLevel:(int)l;
- (int)level;
- (void)ping:(NSNotification *)n;
@end

@implementation Agent
@synthesize name = _name;
- (id)initWithLevel:(int)l {
    if ((self = [super init])) _level = l;
    return self;
}
- (int)level { return _level; }
- (int)notified { return _notified; }
- (NSString *)description { return [NSString stringWithFormat:@"<Agent %@ lvl %d>", _name, _level]; }
- (void)ping:(NSNotification *)n { _notified++; }
- (void)dealloc { [_name release]; [super dealloc]; }
@end

@interface Elite : Agent
@end
@implementation Elite
- (int)level { return [super level] * 10; }
@end

int main(void) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSString *s = [NSString stringWithFormat:@"%@ %d %.1f", @"Adam", 2027, 3.5];
    CHECK([s isEqualToString:@"Adam 2027 3.5"]);
    CHECK([s length] == 13);
    NSMutableArray *a = [NSMutableArray array];
    [a addObject:@"sarif"];
    [a addObject:[NSNumber numberWithInt:42]];
    CHECK([a count] == 2 && [[a objectAtIndex:1] intValue] == 42);
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    [d setObject:@"Detroit" forKey:@"city"];
    CHECK([[d objectForKey:@"city"] isEqualToString:@"Detroit"]);
    int count = 0;
    for (id o in a) count += o ? 1 : 0;
    CHECK(count == 2);

    Agent *jensen = [[Agent alloc] initWithLevel:7];
    jensen.name = @"Jensen";
    CHECK([jensen level] == 7);
    CHECK([[jensen name] isEqualToString:@"Jensen"]);
    CHECK([jensen isKindOfClass:[NSObject class]]);
    CHECK([jensen respondsToSelector:@selector(ping:)]);
    CHECK([[jensen description] isEqualToString:@"<Agent Jensen lvl 7>"]);
    Elite *e = [[Elite alloc] initWithLevel:3];
    CHECK([e level] == 30);
    CHECK([e isKindOfClass:[Agent class]]);

    [[NSNotificationCenter defaultCenter] addObserver:jensen selector:@selector(ping:) name:@"DXTest" object:nil];
    [[NSNotificationCenter defaultCenter] postNotificationName:@"DXTest" object:nil];
    CHECK([jensen notified] == 1);
    [[NSNotificationCenter defaultCenter] removeObserver:jensen];

    NSArray *sorted = [[NSArray arrayWithObjects:@"c", @"a", @"b", nil] sortedArrayUsingSelector:@selector(compare:)];
    CHECK([[sorted componentsJoinedByString:@","] isEqualToString:@"a,b,c"]);

    BOOL caught = NO;
    @try {
        [NSException raise:@"DXError" format:@"bad %d", 1];
    } @catch (NSException *ex) {
        caught = [[ex name] isEqualToString:@"DXError"] && [[ex reason] isEqualToString:@"bad 1"];
    }
    CHECK(caught);
    caught = NO;
    @try {
        [a objectAtIndex:99]; // host exception
    } @catch (NSException *ex) {
        caught = YES;
    }
    CHECK(caught);

    [jensen release];
    [e release];
    [pool drain];
    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL PASSED");
    return fails;
}
