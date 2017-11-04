################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../include/test/hle.c \
../include/test/hle2.c \
../include/test/rtm-goto-test.c \
../include/test/rtm-test.c 

OBJS += \
./include/test/hle.o \
./include/test/hle2.o \
./include/test/rtm-goto-test.o \
./include/test/rtm-test.o 

C_DEPS += \
./include/test/hle.d \
./include/test/hle2.d \
./include/test/rtm-goto-test.d \
./include/test/rtm-test.d 


# Each subdirectory must supply rules for building sources it contributes
include/test/%.o: ../include/test/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


