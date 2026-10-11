// AVAssetReader like Feral's FRLAVFMovie: passthrough outputs for every audio
// track plus a '2vuy' video output, read round-robin. Movie path in argv[1]
// or DXH_TEST_MOVIE.
#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <stdio.h>
#include <stdlib.h>

// Minimal declarations (AVFoundation's headers do not build for i386 any more).
@interface NSObject (AVReaderDecls)
+ (id)assetWithURL:(NSURL *)url;
+ (id)assetReaderWithAsset:(id)asset error:(NSError **)err;
+ (id)assetReaderTrackOutputWithTrack:(id)track outputSettings:(NSDictionary *)settings;
- (NSArray *)tracksWithMediaType:(NSString *)type;
- (void)addOutput:(id)output;
- (BOOL)startReading;
- (NSInteger)status;
- (NSError *)error;
- (CMSampleBufferRef)copyNextSampleBuffer;
@end

extern NSString *const AVMediaTypeVideo;
extern NSString *const AVMediaTypeAudio;

int main(int argc, char **argv) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    {
        const char *path = argc > 1 ? argv[1] : getenv("DXH_TEST_MOVIE");
        if (!path) { printf("SKIP (no movie)\n"); return 0; }
        Class AVAsset = NSClassFromString(@"AVAsset"), Reader = NSClassFromString(@"AVAssetReader"),
              Output = NSClassFromString(@"AVAssetReaderTrackOutput");
        id asset = [AVAsset assetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]];
        NSError *err = nil;
        id reader = [Reader assetReaderWithAsset:asset error:&err];
        NSArray *audio = [asset tracksWithMediaType:AVMediaTypeAudio];
        NSMutableArray *outs = [NSMutableArray arrayWithCapacity:audio.count + 1];
        for (id t in audio) {
            id o = [Output assetReaderTrackOutputWithTrack:t outputSettings:nil];
            [reader addOutput:o];
            [outs addObject:o];
        }
        NSArray *video = [asset tracksWithMediaType:AVMediaTypeVideo];
        printf("tracks: %lu audio, %lu video\n", (unsigned long)audio.count, (unsigned long)video.count);
        NSDictionary *settings = @{(id)kCVPixelBufferPixelFormatTypeKey: [NSNumber numberWithUnsignedInt:'2vuy']};
        id vo = [Output assetReaderTrackOutputWithTrack:[video objectAtIndex:0] outputSettings:settings];
        [reader addOutput:vo];
        BOOL started = [reader startReading];
        printf("startReading %d status %ld\n", started, (long)[reader status]);
        int frames = 0, audio_samples = 0, nonzero = 0;
        for (int iter = 0; iter < 200 && frames < 30; iter++) {
            for (id o in outs) {
                CMSampleBufferRef sb = [o copyNextSampleBuffer];
                if (sb) { audio_samples++; CFRelease(sb); }
            }
            CMSampleBufferRef sb = [vo copyNextSampleBuffer];
            if (!sb) continue;
            CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
            if (img) {
                CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
                const unsigned char *p = CVPixelBufferGetBaseAddress(img);
                size_t n = CVPixelBufferGetBytesPerRow(img) * CVPixelBufferGetHeight(img);
                for (size_t i = 0; p && i < n; i += 997) if (p[i]) { nonzero++; break; }
                CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
                frames++;
            }
            CFRelease(sb);
        }
        printf("video frames %d (with data %d), audio samples %d, status %ld %s\n", frames, nonzero, audio_samples,
               (long)[reader status], [[[reader error] description] UTF8String] ?: "");
        printf("%s\n", frames > 0 && nonzero > 0 ? "PASS" : "FAIL");
        int rc = frames > 0 && nonzero > 0 ? 0 : 1;
        [pool release];
        return rc;
    }
}
