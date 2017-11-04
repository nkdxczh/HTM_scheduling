################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../trials/htm1.cpp \
../trials/htm2.cpp 

C_SRCS += \
../trials/assert-test.c 

OBJS += \
./trials/assert-test.o \
./trials/htm1.o \
./trials/htm2.o 

C_DEPS += \
./trials/assert-test.d 

CPP_DEPS += \
./trials/htm1.d \
./trials/htm2.d 


# Each subdirectory must supply rules for building sources it contributes
trials/%.o: ../trials/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

trials/%.o: ../trials/%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


