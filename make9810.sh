#!/bin/bash
## Exynos 9810 Endurance Kernel Build Script by @Eamo5 & Modified by THEBOSS
## Read before usage!

## Syntax
# For help, run $ ./make9810.sh help
# $ ./make9810.sh <device-varant> <branch> <gcc-version> <oc / uc> <test / release>
# eg. $ ./make9810.sh starlte oreo gcc-4.9 test

## To add:
# * Variable increment when using release parameter
# * Add flexibility to where parameters are entered
# * Work around messy outcomes highlighted in script
# * N9 UC support
# * Detect uncommited work and present confirmation prompt
# * Provide flexibility to other systems and allow for script to be placed in root of source

## Assign Variables
# Correctly assigning these variables makes the script relatively portable. 
# This is useful for me, as I need to compile on multiple systems. This will also aide anyone else trying to compile the kernel.
# File paths *must* have an '/' at the end, or else the script will generate various errors

# Versions
OLD_UPSTREAM=4\.9\.168
UPSTREAM=4\.9\.169
PIE_OLD_VERSION=2\.0\.10
PIE_VERSION=2\.0\.11
OREO_OLD_VERSION=1\.2\.16
OREO_VERSION=1\.3\.107

# Default Device
# Used as a backup when no valid device is defined
DEFAULT_DEVICE=crownlte
DEFAULT_DEVICE_DIRECTORY="$CROWN_KERNEL_DIRECTORY"

# Kernel Source Paths
CROWN_KERNEL_DIRECTORY=/home/theboss/kernels/android-4.9-q/
TOOLCHAINS_DIRECTORY=/home/theboss/kernels/android-4.9-q/toolchains/

# Android Image Kitchen paths
AIK_OREO_N960=/home/theboss/kernels/TW-N960/

# Zip directories
ZIP_OREO_N960=/home/theboss/kernels/zip-note/

# Password for AIK sudo
PASSWORD=

## Functions
# This function should help simplify and compress the code a little bit.
# When passing parameters, the following syntax should be following.
# $ sed_func "OLD_STRING" "NEW_STRING" "PATH"
sed_func () {
	sed -i "s/"$1";/"$2";/g" "$3"
}

pie_oc_crown () {
	sed -i 's/upscale_ratio_table = < 55 962000 65 1261000 90 >;/upscale_ratio_table = < 55 962000 65 1261000 80 >;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
	sed -i 's/unsigned long arg_cpu_max_c2 = 2704000;/unsigned long arg_cpu_max_c2 = 2964000;/g' "$CROWN_KERNEL_DIRECTORY"drivers/cpufreq/exynos-acme.c
	sed -i 's/static unsigned long arg_cpu_max_c1 = 1794000;/static unsigned long arg_cpu_max_c1 = 2002000;/g' "$CROWN_KERNEL_DIRECTORY"drivers/cpufreq/exynos-acme.c
#	sed -i 's/quad_freq = <1794000>;/quad_freq = <2106000>;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
#	sed -i 's/triple_freq = <1794000>;/triple_freq = <2106000>;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
#	sed -i 's/dual_freq = <2314000>;/dual_freq = <2496000>;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
	sed -i 's/2158000/2106000/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
}

oreo_oc_crown () {
	sed -i 's/upscale_ratio_table = < 55 962000 65 1261000 90 >;/upscale_ratio_table = < 75 962000 80 1066000 85 1170000 90 1261000 95 >;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
#	sed -i 's/unsigned long arg_cpu_max_c2 = 2704000;/unsigned long arg_cpu_max_c2 = 2964000;/g' "$CROWN_KERNEL_DIRECTORY"drivers/cpufreq/exynos-acme.c
#	sed -i 's/static unsigned long arg_cpu_max_c1 = 1794000;/static unsigned long arg_cpu_max_c1 = 2002000;/g' "$CROWN_KERNEL_DIRECTORY"drivers/cpufreq/exynos-acme.c
#	sed -i 's/quad_freq = <1794000>;/quad_freq = <2106000>;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
#	sed -i 's/triple_freq = <1794000>;/triple_freq = <2106000>;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
#	sed -i 's/dual_freq = <2314000>;/dual_freq = <2496000>;/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
#	sed -i 's/2158000/2106000/g' "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
}

bsdiff_func () {
	bsdiff boot-G965F.img boot-G960F.img boot-G960F.img.bsdiff
	bsdiff boot-G960F-oc.img boot-G960F.img boot-G960F-oc.img.bsdiff
	bsdiff boot-G960F-uc.img boot-G960F.img boot-G960F-uc.img.bsdiff
	bsdiff boot-G965F-oc.img boot-G965F.img boot-G965F-oc.img.bsdiff
	bsdiff boot-G965F-uc.img boot-G965F.img boot-G965F-uc.img.bsdiff
}

## Help

if [ "$1" == "help" ] || [ "$1" == "" ]; then
	echo ""
	echo "Howdy!"
	echo ""
	echo "How to use this script:"
	echo ""
	echo "1. Assign Variables"
	echo ""
	echo "Correctly assigning variables at the start of the script makes the script relatively portable."
	echo "This means the entire build script can be pretty easily adapted to a different system. eg. Yours!"
	echo "To assign variables you can directly modify this script (make9810.sh)"
	echo "The default device assigned is starlte."
	echo ""
	echo "2. Syntax"
	echo ""
	echo "$ ./make9810.sh <device-varant> <branch> <gcc-version> <oc / uc> <test / release> <update>"
	echo "<oc / uc> and <test / release> arguments are optional."
	echo ""
	echo "3. Compatibility"
	echo ""
	echo "Valid Devices - 'starlte' 'star2lte' 'crownlte' - Not setting a valid device will get you as far as kernels being compiled if the scripts assumptions of the default device are correct. AIK and zipping *must* be done manually"
	echo "Valid Branches - 'pie' 'oreo' 'apgk' 'els' '9.0-aosp' 'gsi' '8.1-aosp' - Script will attempt to work with branch by default and will assume your default device from the assigned variables below."
	echo "If you're lazy you can rename a branch and subsitute it in."
	echo "Valid GCC Versions - 'gcc-8' gcc-4.9' - If no valid parameters are passed, the script defaults to the current branch with GCC 4.9."
	echo "Overclocking / Underclocking - 'oc' 'uc' or this can be ignored entirely and test / release can be put in OC / UC's place."
	echo "Update - If passing the update argument, relevant scripts will be updated with the correct kernel and upstream version. This can be run individually or as an additional argument."
	echo ""
	echo "4. Test vs Release"
	echo ""
	echo "Test - If the argument 'release' is passed in place of OC / UC, the script will create relevant patches with only the base boot.img's present and zip it."
	echo "This will only zip the core boot-G965F.img & boot-G960F.img.bsdiff."
	echo ""
	echo "Release - If the argument 'release' is passed after or in place of OC / UC, the script will create relevant patches with all standard, OC & UC boot.img's present and zip it."
	echo "Hence when building the last kernel (in the last call of the script) you'll want to pass 'release'."
	echo "Refer to make9810release.sh for a reference example for how I create a release zip!"
	echo ""
	exit
fi

## Zip Update

if [ "$1" == "update" ] || [ "$2" == "update" ] || [ "$3" == "update" ] || [ "$4" == "update" ] || [ "$5" == "update" ] || [ "$6" == "update" ]; then
	echo "Updating zip installer versions..."
	sed -i "s/"$OLD_UPSTREAM"/"$UPSTREAM"/g" "$ZIP_OREO"META-INF/com/google/android/update-binary
	sed -i "s/"$OLD_UPSTREAM"/"$UPSTREAM"/g" "$ZIP_OREO_AOSP_S9"META-INF/com/google/android/update-binary
	sed -i "s/"$OLD_UPSTREAM"/"$UPSTREAM"/g" "$ZIP_GSI_S9"META-INF/com/google/android/update-binary
	sed -i "s/"$OLD_UPSTREAM"/"$UPSTREAM"/g" "$ZIP_PIE"META-INF/com/google/android/update-binary
	sed -i "s/"$OLD_UPSTREAM"/"$UPSTREAM"/g" "$ZIP_PIE_AOSP_S9"META-INF/com/google/android/update-binary
	sed -i "s/"$OREO_OLD_VERSION"/"$OREO_VERSION"/g" "$ZIP_OREO"META-INF/com/google/android/update-binary
	sed -i "s/"$OREO_OLD_VERSION"/"$OREO_VERSION"/g" "$ZIP_OREO_AOSP_S9"META-INF/com/google/android/update-binary
	sed -i "s/"$OREO_OLD_VERSION"/"$OREO_VERSION"/g" "$ZIP_GSI_S9"META-INF/com/google/android/update-binary
	sed -i "s/"$OREO_OLD_VERSION"/"$OREO_VERSION"/g" "$ZIP_OREO_N960"META-INF/com/google/android/updater-script
	sed -i "s/"$OREO_OLD_VERSION"/"$OREO_VERSION"/g" "$ZIP_GSI_N960"META-INF/com/google/android/updater-script 
	sed -i "s/"$PIE_OLD_VERSION"/"$PIE_VERSION"/g" "$ZIP_PIE"META-INF/com/google/android/update-binary
	sed -i "s/"$PIE_OLD_VERSION"/"$PIE_VERSION"/g" "$ZIP_PIE_N960"META-INF/com/google/android/update-binary
	sed -i "s/Endurance Kernel "$PIE_OLD_VERSION"/Endurance Kernel "$PIE_VERSION"/g" "$ZIP_PIE_N960"META-INF/com/google/android/updater-script
	sed -i "s/Endurance Kernel "$PIE_OLD_VERSION"/Endurance Kernel "$PIE_VERSION"/g" "$ZIP_PIE_AOSP_N960"META-INF/com/google/android/updater-script
	if [ "$1" == "update" ]; then
		exit
	fi
fi

## Kernel Directory

if [ "$1" == "starlte" ] || [ "$1" == "star2lte" ]; then
	cd "$STAR_KERNEL_DIRECTORY" || exit
elif [ "$1" == "crownlte" ]; then
	cd "$CROWN_KERNEL_DIRECTORY" || exit
else
	echo "Did not define a valid specific device!"
	echo "Correct syntax is as follows: ./make9810.sh <device-variant> <branch> <gcc-version> <oc / uc> <test / release>"
	echo "eg. $ ./make9810.sh starlte oreo gcc-4.9 test"
	echo "Defaulting to star kernel directory..."
	cd "$DEFAULT_DEVICE_DIRECTORY" || exit
fi

## Branch Selection
# Note: This will remove all uncommited changes. This is necessary when modifying defconfigs on the fly. If desired, git reset --hard can be removed. A bypass argument may be added eventually.
#
git reset --hard
#if [ "$1" == "starlte" ] || [ "$1" == "star2lte" ]; then
#	if [ "$2" == "pie" ]; then
#		git checkout Endurance-Kernel-P
#		git reset --hard
#	elif [ "$2" == "oreo" ]; then
#		git checkout THEBOSS-Zeus
#		git reset --hard
#	else
#		git checkout "$2"
#		git reset --hard
#	fi
if [ "$1" == "crownlte" ]; then
	if [ "$2" == "pie" ]; then
		git checkout Endurance-Kernel-P
		git reset --hard
	elif [ "$2" == "oreo" ]; then
		git checkout THEBOSS-ZeusQ
		git reset --hard
	else
		git checkout "$2"
		git reset --hard
	fi
else
	git checkout "$2"
	git reset --hard
fi

## Build environment

export ARCH=arm64
export SUBARCH=arm64

# Check for specified GCC parameters. If none passed, use GCC 4.9 by default + use ccache to improve compile speeds

if [ "$3" == "gcc-8" ]; then
	export CROSS_COMPILE="ccache "$TOOLCHAINS_DIRECTORY"gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu/bin/aarch64-linux-gnu-"
elif [ "$3" == "gcc-9.1" ]; then
	export CROSS_COMPILE="ccache "$TOOLCHAINS_DIRECTORY"linaro-64/bin/aarch64-linux-gnu-"
elif [ "$3" == "gcc-10" ]; then
	export CROSS_COMPILE="ccache "$TOOLCHAINS_DIRECTORY"GCC-10/bin/aarch64-linux-elf-"
	export CROSS_COMPILE_ARM32="ccache "$TOOLCHAINS_DIRECTORY"7.1/bin/arm-eabi-"
	export LDLLD="ccache "$TOOLCHAINS_DIRECTORY"DragonTC-CLANG-9.0.5/bin/ld.lld"
	export CC="ccache "$TOOLCHAINS_DIRECTORY"DragonTC-CLANG-9.0.5/bin/clang"
	export CLANG_TRIPLE="ccache "$TOOLCHAINS_DIRECTORY"GCC-10/bin/aarch64-linux-elf-"
elif [ "$3" == "gcc-4.9" ]; then
	export CROSS_COMPILE="ccache "$TOOLCHAINS_DIRECTORY"aarch64-linux-android-4.9/bin/aarch64-linux-android-"
else
	echo "Did not export a known toolchain... Defaulting to GCC 4.9"
	echo "Correct syntax is as follows: $ ./make9810.sh <device-variant> <branch> <gcc-version> <oc / uc> <test / release>"
	echo "For more information, run $ ./make9810.sh help"
	export CROSS_COMPILE="ccache "$TOOLCHAINS_DIRECTORY"gcc-arm-8.3-2019.03-x86_64-aarch64-linux-gnu/bin/aarch64-linux-gnu-"
fi

## Build Specific Preperation
# Includes specific modifications for OC and UC on the kernel, as well as modifies the kernels localversion on the fly.

if [ "$2" == "pie" ]; then
	if [ "$1" == "crownlte" ] && [ "$4" == "oc" ]; then
		sed -i "s/-Endurance-Kernel-N9/-Endurance-Kernel-N9-OC-"$PIE_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/configs/exynos9810-crownlte_defconfig
		sed -i "s/-Endurance-Kernel-N9/-Endurance-Kernel-N9-OC-"$PIE_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/include/dt-bindings/soc/samsung/crown_conf.h
		pie_oc_crown
#	elif [ "$1" == "crownlte" ] && [ "$4" == "uc" ]; then
#		sed -i "s/-Endurance-Kernel-N9/-Endurance-Kernel-N9-UC-"$PIE_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/configs/exynos9810-crownlte_defconfig
#		sed -i "s/-Endurance-Kernel-N9/-Endurance-Kernel-N9-UC-"$PIE_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/include/dt-bindings/soc/samsung/crown_conf.h
#		sed -i 's/quad_freq = <1794000>;/quad_freq = <1586000>;/g' "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/dts/exynos/exynos9810.dtsi
	elif [ "$1" == "crownlte" ] && [ "$4" != "oc" ] && [ "$4" != "uc" ]; then
		sed -i "s/-Endurance-Kernel-N9/-Endurance-Kernel-N9-"$PIE_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/configs/exynos9810-crownlte_defconfig
		sed -i "s/-Endurance-Kernel-N9/-Endurance-Kernel-N9-"$PIE_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/include/dt-bindings/soc/samsung/crown_conf.h
	else
		echo "Invalid device or OC configuration detected... Please check your inputs."
	fi
elif [ "$2" == "oreo" ]; then
	if [ "$1" == "crownlte" ] && [ "$4" == "oc" ]; then
		sed -i "s/-THEBOSS-Zeus-OC-/-THEBOSS-Zeus-OC-N9-"$OREO_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/configs/exynos9810-crownlte_defconfig
		sed -i "s/-THEBOSS-Zeus-OC-/-THEBOSS-Zeus-OC-N9-"$OREO_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/include/dt-bindings/soc/samsung/crown_conf.h
		oreo_oc_crown
	elif [ "$1" == "crownlte" ] && [ "$4" == "oc" ]; then
		sed -i "s/-THEBOSS-Zeus-OC-/-THEBOSS-Zeus-OC-N9-"$OREO_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/configs/exynos9810-crownlte_defconfig
		sed -i "s/-THEBOSS-Zeus-OC-/-THEBOSS-Zeus-OC-N9-"$OREO_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/include/dt-bindings/soc/samsung/crown_conf.h
	elif [ "$1" == "crownlte" ] && [ "$4" != "uc" ]; then
		sed -i "s/-THEBOSS-Zeus-OC-/-THEBOSS-Zeus-OC-N9-"$OREO_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/configs/exynos9810-crownlte_defconfig
		sed -i "s/-THEBOSS-Zeus-OC-/-THEBOSS-Zeus-OC-N9-"$OREO_VERSION"/g" "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dts/include/dt-bindings/soc/samsung/crown_conf.h
	else
		echo "Invalid device or OC configuration detected... Please check your inputs."
	fi
fi

## Make

make -j$(nproc) clean
make -j$(nproc) mrproper
if [ "$2" == "pie" ] || [ "$2" == "els" ] || [ "$2" == "9.0-aosp" ]; then
	if [ "$1" == "crownlte" ]; then
		make -j$(nproc) exynos9810-crownlte_defconfig
	else
		echo "Incorrect device variant configuration..."
		echo "Using default device"
		make -j$(nproc) exynos9810-"$DEFAULT_DEVICE"_defconfig
	fi
elif [ "$2" == "oreo" ] || [ "$2" == "apgk" ] || [ "$2" == "gsi" ] || [ "$2" == "8.1-aosp" ]; then
	if [ "$1" == "crownlte" ]; then
		make -j$(nproc) exynos9810-crownlte_defconfig
	else
		echo "Incorrect device variant configuration..."
		echo "Using default device"
		make -j$(nproc) exynos9810-"$DEFAULT_DEVICE"_defconfig
	fi
else
	echo "Did not define a known branch. Defaulting to using standard default device defconfig."
	make -j$(nproc) exynos9810-"$DEFAULT_DEVICE"_defconfig
fi
make -j$(nproc)

## Cleanup

git reset --hard

## AIK Preparation

if [ "$2" == "pie" ] || [ "$2" == "els" ]; then
	if [ "$1" == "crownlte" ]; then
		rm "$AIK_PIE_N960"split_img/boot.img-dtb
		rm "$AIK_PIE_N960"split_img/boot.img-zImage
		cp "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_PIE_N960"split_img/boot.img-zImage
		cp "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_PIE_N960"split_img/boot.img-dtb
		cd "$AIK_PIE_N960" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		cp "$AIK_PIE_N960"image-new.img "$ZIP_PIE_N960"boot.img
	fi
elif [ "$2" == "oreo" ] || [ "$2" == "apgk" ]; then
	if [ "$1" == "crownlte" ]; then
		rm "$AIK_OREO_N960"split_img/boot.img-dtb
		rm "$AIK_OREO_N960"split_img/boot.img-zImage
		cp "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_OREO_N960"split_img/boot.img-zImage
		cp "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_OREO_N960"split_img/boot.img-dtb
		cd "$AIK_OREO_N960" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		cp "$AIK_OREO_N960"image-new.img  "$ZIP_OREO_N960"boot.img
	fi
elif [ "$2" == "9.0-aosp" ]; then
	rm "$AIK_PIE_AOSP"split_img/boot.img-dtb
	rm "$AIK_PIE_AOSP"split_img/boot.img-zImage
	cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_PIE_AOSP"split_img/boot.img-zImage
	cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_PIE_AOSP"split_img/boot.img-dtb
	cd "$AIK_PIE_AOSP" || exit
	echo "$PASSWORD" | sudo -S ./repackimg.sh
	if [ "$1" == "crownlte" ]; then
		cp "$AIK_PIE_AOSP"image-new.img "$ZIP_PIE_AOSP_N960"boot.img
	elif [ "$1" == "starlte" ]; then
		if [ "$4" != "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_PIE_AOSP"image-new.img "$ZIP_PIE_AOSP_S9"boot-G960F.img
		elif [ "$4" == "oc" ]; then
			cp "$AIK_PIE_AOSP"image-new.img "$ZIP_PIE_AOSP_S9"boot-G960F-oc.img
		elif [ "$4" == "uc" ]; then
			cp "$AIK_PIE_AOSP"image-new.img "$ZIP_PIE_AOSP_S9"boot-G960F-uc.img
		fi
	elif [ "$1" == "star2lte" ]; then
		if [ "$4" != "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_PIE_AOSP"image-new.img  "$ZIP_PIE_AOSP_S9"boot-G965F.img
		elif [ "$4" == "oc" ]; then
			cp "$AIK_PIE_AOSP"image-new.img  "$ZIP_PIE_AOSP_S9"boot-G965F-oc.img
		elif [ "$4" == "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_PIE_AOSP"image-new.img  "$ZIP_PIE_AOSP_S9"boot-G965F-uc.img
		fi
	fi
elif [ "$2" == "gsi" ]; then
	if [ "$1" == "starlte" ]; then
		rm "$AIK_OREO_960"split_img/boot.img-dtb
		rm "$AIK_OREO_960"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_OREO_960"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_OREO_960"split_img/boot.img-dtb
		cd "$AIK_OREO_960" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		if [ "$4" != "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_OREO_960"image-new.img "$ZIP_GSI_S9"boot-G960F.img
		elif [ "$4" == "oc" ]; then
			cp "$AIK_OREO_960"image-new.img "$ZIP_GSI_S9"boot-G960F-oc.img
		elif [ "$4" == "uc" ]; then
			cp "$AIK_OREO_960"image-new.img "$ZIP_GSI_S9"boot-G960F-uc.img
		fi
	elif [ "$1" == "star2lte" ]; then
		rm "$AIK_OREO_965"split_img/boot.img-dtb
		rm "$AIK_OREO_965"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_OREO_965"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_OREO_965"split_img/boot.img-dtb
		cd "$AIK_OREO_965" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		if [ "$4" != "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_OREO_965"image-new.img "$ZIP_GSI_S9"boot-G965F.img
		elif [ "$4" == "oc" ]; then
			cp "$AIK_OREO_965"image-new.img "$ZIP_GSI_S9"boot-G965F-oc.img
		elif [ "$4" == "uc" ]; then
			cp "$AIK_OREO_965"image-new.img "$ZIP_GSI_S9"boot-G965F-uc.img
		fi
	elif [ "$1" == "crownlte" ]; then
		rm "$AIK_OREO_N960"split_img/boot.img-dtb
		rm "$AIK_OREO_N960"split_img/boot.img-zImage
		cp "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_OREO_N960"split_img/boot.img-zImage
		cp "$CROWN_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_OREO_N960"split_img/boot.img-dtb
		cd "$AIK_OREO_N960" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		cp "$AIK_OREO_N960"image-new.img  "$ZIP_GSI_N960"boot.img
	fi
elif [ "$2" == "8.1-aosp" ]; then
	if [ "$1" == "starlte" ]; then
		rm "$AIK_OREO_AOSP_960"split_img/boot.img-dtb
		rm "$AIK_OREO_AOSP_960"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_OREO_AOSP_960"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_OREO_AOSP_960"split_img/boot.img-dtb
		cd "$AIK_OREO_AOSP_960" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		if [ "$4" != "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_OREO_AOSP_960"image-new.img  "$ZIP_OREO_AOSP_S9"boot-G960F.img
		elif [ "$4" == "oc" ]; then
			cp "$AIK_OREO_AOSP_960"image-new.img  "$ZIP_OREO_AOSP_S9"boot-G960F-oc.img
		elif [ "$4" == "uc" ]; then
			cp "$AIK_OREO_AOSP_960"image-new.img  "$ZIP_OREO_AOSP_S9"boot-G960F-uc.img
		fi
	elif [ "$1" == "star2lte" ]; then
		rm "$AIK_OREO_AOSP_965"split_img/boot.img-dtb
		rm "$AIK_OREO_AOSP_965"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/Image "$AIK_OREO_AOSP_965"split_img/boot.img-zImage
		cp "$STAR_KERNEL_DIRECTORY"arch/arm64/boot/dtb.img "$AIK_OREO_AOSP_965"split_img/boot.img-dtb
		cd "$AIK_OREO_AOSP_965" || exit
		echo "$PASSWORD" | sudo -S ./repackimg.sh
		if [ "$4" != "uc" ] && [ "$4" != "oc" ]; then
			cp "$AIK_OREO_AOSP_965"image-new.img  "$ZIP_OREO_AOSP_S9"boot-G965F.img
		elif [ "$4" == "oc" ]; then
			cp "$AIK_OREO_AOSP_965"image-new.img  "$ZIP_OREO_AOSP_S9"boot-G965F-oc.img
		elif [ "$4" == "uc" ]; then
			cp "$AIK_OREO_AOSP_965"image-new.img  "$ZIP_OREO_AOSP_S9"boot-G965F-uc.img
		fi
	fi
else
	echo "This is where we depart. You're on your own for AIK!"
	exit
fi

# If 'test' or 'release' aren't passed, the script ends here. Your kernel should be in the zip folder, or at least the relevant AIK folder.

## Test Kernel
# If the argument 'release' is passed in place of OC / UC, the script will create relevant patches with the base boot.img's present and zip it.
# This will only zip the core boot-G965F.img & boot-G960F.img.bsdiff

if [ "$4" == "test" ] || [ "$5" == "test" ]; then
	if [ "$1" == "starlte" ] || [ "$1" == "star2lte" ]; then
		if [ "$2" == "pie" ] || [ "$2" == "els" ]; then
			cd "$ZIP_PIE" || exit
			bsdiff boot-G965F.img boot-G960F.img boot-G960F.img.bsdiff
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-"$PIE_VERSION".zip *
		elif [ "$2" == "oreo" ] || [ "$2" == "apgk" ]; then
			cd "$ZIP_OREO" || exit
			bsdiff boot-G965F.img boot-G960F.img boot-G960F.img.bsdiff
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-"$OREO_VERSION".zip *
		elif [ "$2" == "9.0-aosp" ]; then
			cd "$ZIP_PIE_AOSP_S9" || exit
			bsdiff boot-G965F.img boot-G960F.img boot-G960F.img.bsdiff
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-AOSP-"$PIE_VERSION".zip *
		elif [ "$2" == "gsi" ]; then
			cd "$ZIP_GSI_S9" || exit
			bsdiff boot-G965F.img boot-G960F.img boot-G960F.img.bsdiff
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-GSI-"$OREO_VERSION".zip *
		elif [ "$2" == "8.1-aosp" ]; then
			cd "$ZIP_OREO_AOSP_S9" || exit
			bsdiff boot-G965F.img boot-G960F.img boot-G960F.img.bsdiff
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-AOSP-"$OREO_VERSION".zip *
		fi
	elif [ "$1" == "crownlte" ]; then
		if [ "$2" == "pie" ] || [ "$2" == "els" ]; then
			cd "$ZIP_PIE_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-"$PIE_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-"$PIE_VERSION".zip *
			fi
		elif [ "$2" == "oreo" ]; then
			cd "$ZIP_OREO_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-"$OREO_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-"$OREO_VERSION".zip *
			fi
		elif [ "$2" == "9.0-aosp" ]; then
			cd "$ZIP_PIE_AOSP_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-AOSP-"$PIE_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-AOSP-"$PIE_VERSION".zip *
			fi
		elif [ "$2" == "gsi" ]; then
			cd "$ZIP_GSI_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-GSI-"$OREO_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-GSI-"$OREO_VERSION".zip *
			fi
		fi
	fi
fi

## Kernel Release
# If the argument 'release' is passed after or in place of OC / UC, the script will create relevant patches with all boot.img's present and zip it.
# Hence when building the last kernel (in the last call of the script) you'll want to pass 'release'.
# Refer to make9810release.sh for a reference example for how I create a release zip!

if [ "$4" == "release" ] || [ "$5" == "release" ]; then
	if [ "$1" == "starlte" ] || [ "$1" == "star2lte" ]; then
		if [ "$2" == "pie" ] || [ "$2" == "els" ]; then
			cd "$ZIP_PIE" || exit
			bsdiff_func
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-"$PIE_VERSION".zip *
		elif [ "$2" == "oreo" ] || [ "$2" == "apgk" ]; then
			cd "$ZIP_OREO" || exit
			bsdiff_func
    		rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-"$OREO_VERSION".zip *
		elif [ "$2" == "9.0-aosp" ]; then
			cd "$ZIP_PIE_AOSP_S9" || exit
			bsdiff_func
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-AOSP-"$PIE_VERSION".zip *
		elif [ "$2" == "gsi" ]; then
			cd "$ZIP_GSI_S9" || exit
			bsdiff_func
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-GSI-"$OREO_VERSION".zip *
		elif [ "$2" == "8.1-aosp" ]; then
			cd "$ZIP_OREO_AOSP_S9" || exit
			bsdiff_func
			rm -f boot-G960F.img boot-G960F-uc.img boot-G960F-oc.img boot-G965F-oc.img boot-G965F-uc.img *.zip
			zip -r Endurance-Kernel-AOSP-"$OREO_VERSION".zip *
		fi
	elif [ "$1" == "crownlte" ]; then
		if [ "$2" == "pie" ] || [ "$2" == "els" ]; then
			cd "$ZIP_PIE_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-"$PIE_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-"$PIE_VERSION".zip *
			fi
		elif [ "$2" == "oreo" ]; then
			cd "$ZIP_OREO_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-"$OREO_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-"$OREO_VERSION".zip *
			fi
		elif [ "$2" == "9.0-aosp" ]; then
			cd "$ZIP_PIE_AOSP_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-AOSP-"$PIE_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-AOSP-"$PIE_VERSION".zip *
			fi
		elif [ "$2" == "gsi" ]; then
			cd "$ZIP_GSI_N960" || exit
			rm -f *.zip
			if [ "$4" == "oc" ]; then
				zip -r Endurance-Kernel-GSI-"$OREO_VERSION"-OC.zip *
			else
				zip -r Endurance-Kernel-GSI-"$OREO_VERSION".zip *
			fi
		fi
	fi
fi
