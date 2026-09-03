Requirements for building for iOS on macOS
=========================================

-	Xcode with the iOS SDK and command line tools installed.
-	CMake 3.20 or newer.
-	Rust (stable). For reproducible builds, use the same version as the CI.
-	Install the iOS Rust targets:
	```shell
	rustup target add aarch64-apple-ios
	rustup target add aarch64-apple-ios-sim
	rustup target add x86_64-apple-ios
	```
-	Build the `ddnet-libs` for iOS (see below) or use precompiled ones from https://github.com/ddnet/ddnet-libs/.

How to locally build the `ddnet-libs` for iOS
=============================================

-	Install dependencies:
	```shell
	brew install autoconf automake cmake libtool m4 ninja pkg-config
	```
-	Run the iOS library build script:
	```shell
	scripts/compile_libs/gen_libs.sh build-ios-libs ios
	```
	**Warning**: Do not choose a directory inside the `src` folder!
-	After the script finished executing, it should have created a `ddnet-libs` directory
	in your selected output folder, which contains all libraries in the correct directory
	format and can be merged with the `ddnet-libs` folder in the source directory:
	```shell
	find ddnet-libs -type d -name ios -exec rm -r {} + -prune
	cp -r build-ios-libs/ddnet-libs/. ddnet-libs/
	```

How to build the DDNet client for iOS
=====================================

-	Open a terminal inside the project root and run:
	```shell
	scripts/ios/cmake_ios.sh <device/sim-arm64/sim-x86_64/sim> <App name> <Bundle id> <Debug/Release> <Build folder>
	```
	- `device` builds an arm64 iPhoneOS app.
	- `sim` uses the host architecture to build the iOS simulator app.
-	Example to build a simulator app on Apple Silicon:
	```shell
	scripts/ios/cmake_ios.sh sim DDNet org.ddnet.client Debug build-ios-sim
	```
-	To build for a physical device, you need signing configured in Xcode.
	You can open the generated Xcode project in the build folder for signing,
	or provide signing settings via Xcode environment variables.

Notes
=====

-	The iOS build uses bundled libraries by default and disables server, tools,
	Vulkan, and the video recorder to match mobile constraints.
-	The app bundles the `data` folder directly into the app package and reads
	the files from there at runtime. User data is stored in the sandboxed
	Application Support directory.
-	Start with a recent iOS deployment target by exporting `IOS_DEPLOYMENT_TARGET`
	(e.g. `IOS_DEPLOYMENT_TARGET=17.0`) and only lower it after the app runs
	correctly on the latest iOS.
