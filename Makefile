RM := rm -rf

# All of the sources participating in the build are defined here
CPP_SRCS := \
grabcompressandsend.cpp \
receivedecompressandplay.cpp

C_SRCS := \
time.c \
vpx_network.c

OBJS := \
time.o \
vpx_network.o 

CPP_DEPS := \
./grabcompressandsend.d \
./receivedecompressandplay.d

C_DEPS := \
./time.d \
./vpx_network.d 

UNAME := $(shell uname)

ifeq ($(UNAME), Linux)
    C_FLAGS = -DLINUX  -O0 -g3 -Wall -c -fmessage-length=0 -m64 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)"
    RLIBS := -lvpx -lpthread -lrt -lSDL 
    SLIBS := -lvpx -lpthread -lrt 
    L_FLAGS := -m64  
else
ifeq ($(UNAME), Darwin)
    C_FLAGS = -DLINUX -DMACOSX  -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)"
    RLIBS := -lvpx -lpthread -lSDL -lpthread -lSDLmain -framework cocoa
    SLIBS := -framework Carbon -framework QuartzCore -framework QuickTime -lvpx -lpthread -framework cocoa -lvidcap
    L_FLAGS := -D_THREAD_SAFE
else
ifneq ($(findstring CYGWIN, $(UNAME)),)
    C_FLAGS = -DLINUX -DMACOSX -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)"
    RLIBS := -lvpx -lpthread -lrt -lSDL 
    SLIBS := -lvpx -lpthread -lrt -lvidcap
    L_FLAGS := 
else
    $(error Unknown System need to fix this make file!)
endif
endif
endif
EXECUTABLES := grabcompressandsend receivedecompressandplay 


# Each subdirectory must supply rules for building sources it contributes
%.o: %.cpp
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C++ Compiler'
	g++ $(C_FLAGS) -o"$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

%.o: %.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc $(C_FLAGS) -o"$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '



# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all: grabcompressandsend receivedecompressandplay

# Tool invocations
grabcompressandsend: $(OBJS) $(USER_OBJS) ./grabcompressandsend.o 
	@echo 'Building target: $@'
	@echo 'Invoking: GCC C++ Linker'
	@echo g++ $(L_FLAGS) -o "grabcompressandsend" ./grabcompressandsend.o $(OBJS) $(SLIBS)
	g++ $(L_FLAGS) -o "grabcompressandsend" ./grabcompressandsend.o $(OBJS) $(SLIBS)
	@echo 'Finished building target: $@'
	@echo ' '

receivedecompressandplay: $(OBJS) $(USER_OBJS) ./receivedecompressandplay.o 
	@echo 'Building target: $@'
	@echo 'Invoking: GCC C++ Linker'
	g++ $(L_FLAGS) -o "receivedecompressandplay" ./receivedecompressandplay.o $(OBJS) $(RLIBS)
	@echo 'Finished building target: $@'
	@echo ' '


# Other Targets
clean:
	-$(RM) $(OBJS) $(C_DEPS) $(CPP_DEPS) $(EXECUTABLES) receivedecompressandplay.o grabcompressandsend.o
	-@echo ' '


