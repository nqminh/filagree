#import "AppDelegate.h"
#include "interpret.h"


@implementation AppDelegate


- (void)applicationDidFinishLaunching:(NSNotification *)aNotification {
    struct byte_array *filename = byte_array_from_string("sync_client.fg");
    interpret_file(filename, NULL);
}

@end