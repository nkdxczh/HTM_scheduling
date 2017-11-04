################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
CPP_SRCS += \
../libs/lockfree/test/bench_1.cpp \
../libs/lockfree/test/fifo_test.cpp \
../libs/lockfree/test/freelist_test.cpp \
../libs/lockfree/test/ringbuffer_test.cpp \
../libs/lockfree/test/stack_test.cpp \
../libs/lockfree/test/tagged_ptr_test.cpp 

OBJS += \
./libs/lockfree/test/bench_1.o \
./libs/lockfree/test/fifo_test.o \
./libs/lockfree/test/freelist_test.o \
./libs/lockfree/test/ringbuffer_test.o \
./libs/lockfree/test/stack_test.o \
./libs/lockfree/test/tagged_ptr_test.o 

CPP_DEPS += \
./libs/lockfree/test/bench_1.d \
./libs/lockfree/test/fifo_test.d \
./libs/lockfree/test/freelist_test.d \
./libs/lockfree/test/ringbuffer_test.d \
./libs/lockfree/test/stack_test.d \
./libs/lockfree/test/tagged_ptr_test.d 


# Each subdirectory must supply rules for building sources it contributes
libs/lockfree/test/%.o: ../libs/lockfree/test/%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


