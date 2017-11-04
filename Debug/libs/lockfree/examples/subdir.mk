################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../libs/lockfree/examples/fifo.cpp \
../libs/lockfree/examples/ringbuffer.cpp \
../libs/lockfree/examples/stack.cpp 

OBJS += \
./libs/lockfree/examples/fifo.o \
./libs/lockfree/examples/ringbuffer.o \
./libs/lockfree/examples/stack.o 

CPP_DEPS += \
./libs/lockfree/examples/fifo.d \
./libs/lockfree/examples/ringbuffer.d \
./libs/lockfree/examples/stack.d 


# Each subdirectory must supply rules for building sources it contributes
libs/lockfree/examples/%.o: ../libs/lockfree/examples/%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


