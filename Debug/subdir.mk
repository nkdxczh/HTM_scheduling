################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
O_SRCS += \
../TPCcHTM.o \
../assert-test.o \
../htm.o \
../htm_bank.o \
../htm_conflict.o \
../htm_rob_sched.o \
../htm_sched.o \
../tsx-assert.o 

CPP_SRCS += \
../TPCcHTM.cpp \
../htm.cpp \
../htm_bank.cpp \
../htm_conflict.cpp \
../htm_rob_sched.cpp \
../htm_sched.cpp \
../htm_sched_1st.cpp \
../htm_sched_q_top.cpp \
../htm_sched_versionKevin.McGee.cpp 

C_SRCS += \
../assert-test.c \
../assert-test2.c \
../has-tsx.c \
../ignore-xend.c \
../tsx-assert.c 

OBJS += \
./TPCcHTM.o \
./assert-test.o \
./assert-test2.o \
./has-tsx.o \
./htm.o \
./htm_bank.o \
./htm_conflict.o \
./htm_rob_sched.o \
./htm_sched.o \
./htm_sched_1st.o \
./htm_sched_q_top.o \
./htm_sched_versionKevin.McGee.o \
./ignore-xend.o \
./tsx-assert.o 

C_DEPS += \
./assert-test.d \
./assert-test2.d \
./has-tsx.d \
./ignore-xend.d \
./tsx-assert.d 

CPP_DEPS += \
./TPCcHTM.d \
./htm.d \
./htm_bank.d \
./htm_conflict.d \
./htm_rob_sched.d \
./htm_sched.d \
./htm_sched_1st.d \
./htm_sched_q_top.d \
./htm_sched_versionKevin.McGee.d 


# Each subdirectory must supply rules for building sources it contributes
%.o: ../%.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

%.o: ../%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


