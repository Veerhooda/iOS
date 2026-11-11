# op2gpu: the Adreno 430's command-processor layer for userland (G1), its host tests and the phone probe.
#   make test     build and run the tests on this Mac against the fake GPU
#   make ios      the probe for iOS 12 arm64 (Xcode 10.3's iPhoneOS 12.4 SDK), out/ios/op2gpu-probe
CXX      ?= c++
CXXFLAGS ?= -O2 -g
override CXXFLAGS += -std=c++17 -Wall -Wextra -Werror -Iinclude
CORE     = src/gpu.cpp src/blit.cpp src/checks.cpp
XCODE    ?= /Users/veerhooda/xcode-10.3/Xcode.app
SDK      ?= $(XCODE)/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS12.4.sdk
IOS_CXX  ?= clang++ -target arm64-apple-ios12.0 -isysroot $(SDK)

.PHONY: test ios clean
test: out/host/tests
	out/host/tests

out/host/tests: $(CORE) src/fake_gpu.cpp tests/main.cpp $(wildcard include/op2gpu/*.hpp)
	mkdir -p out/host
	$(CXX) $(CXXFLAGS) $(CORE) src/fake_gpu.cpp tests/main.cpp -o $@

ios: out/ios/op2gpu-probe

out/ios/op2gpu-probe: $(CORE) src/iokit_transport.cpp tools/probe.cpp $(wildcard include/op2gpu/*.hpp)
	mkdir -p out/ios
	$(IOS_CXX) -std=c++17 -Os -Wall -Wextra -Werror -Iinclude -fvisibility=hidden $(CORE) src/iokit_transport.cpp tools/probe.cpp \
	  -framework IOKit -framework CoreFoundation -o $@
	strip -x $@
	codesign -s - -i com.apple.tzinit --force $@
	@ls -l $@

clean:
	rm -rf out
