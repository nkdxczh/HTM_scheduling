################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../benchmarks/htm.cpp \
../benchmarks/htm_Bank.cpp \
../benchmarks/htm_Bank2.cpp \
../benchmarks/htm_Bank3.cpp \
../benchmarks/htm_HTM_GL2.cpp \
../benchmarks/htm_HTM_GLOCK.cpp \
../benchmarks/htm_N_ReadWrite.cpp \
../benchmarks/htm_trial1.cpp \
../benchmarks/htm_trial2.cpp 

OBJS += \
./benchmarks/htm.o \
./benchmarks/htm_Bank.o \
./benchmarks/htm_Bank2.o \
./benchmarks/htm_Bank3.o \
./benchmarks/htm_HTM_GL2.o \
./benchmarks/htm_HTM_GLOCK.o \
./benchmarks/htm_N_ReadWrite.o \
./benchmarks/htm_trial1.o \
./benchmarks/htm_trial2.o 

CPP_DEPS += \
./benchmarks/htm.d \
./benchmarks/htm_Bank.d \
./benchmarks/htm_Bank2.d \
./benchmarks/htm_Bank3.d \
./benchmarks/htm_HTM_GL2.d \
./benchmarks/htm_HTM_GLOCK.d \
./benchmarks/htm_N_ReadWrite.d \
./benchmarks/htm_trial1.d \
./benchmarks/htm_trial2.d 


# Each subdirectory must supply rules for building sources it contributes
benchmarks/%.o: ../benchmarks/%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


