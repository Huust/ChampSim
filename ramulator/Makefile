SRCDIR := src
OBJDIR := obj
MAIN := $(SRCDIR)/Main.cpp
SRCS := $(filter-out $(MAIN) $(SRCDIR)/Gem5Wrapper.cpp, $(wildcard $(SRCDIR)/*.cpp))
OBJS := $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRCS))


# Ramulator currently supports g++ 5.1+ or clang++ 3.4+.  It will NOT work with
#   g++ 4.x due to an internal compiler error when processing lambda functions.
CXX := g++
# CXX := g++-5
# Suppress unused lambda capture warnings from template instantiations
# and unused variable warnings from debug/test code
CXXFLAGS := -O3 -std=c++17 -g -Wall -Wno-unused-lambda-capture -Wno-unused-but-set-variable
AR = ar
ARFLAGS = rcs

.PHONY: all clean depend

all: depend ramulator libramulator.a

clean:
	@rm -f ramulator
	@rm -rf $(OBJDIR)
	@rm -f libramulator.a

depend: $(OBJDIR)/.depend


$(OBJDIR)/.depend: $(SRCS)
	@mkdir -p $(OBJDIR)
	@rm -f $(OBJDIR)/.depend
	@$(foreach SRC, $(SRCS), $(CXX) $(CXXFLAGS) -DRAMULATOR $(USER_DEFINES) -MM -MT $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRC)) $(SRC) >> $(OBJDIR)/.depend ;)

ifneq ($(MAKECMDGOALS),clean)
-include $(OBJDIR)/.depend
endif


ramulator: $(MAIN) $(OBJS) $(SRCDIR)/*.h | depend
	$(CXX) $(CXXFLAGS) -DRAMULATOR $(USER_DEFINES) -o $@ $(MAIN) $(OBJS)

# libramulator.a: $(OBJS) $(OBJDIR)/Gem5Wrapper.o
# 	libtool  --mode=link -o $@ $(OBJS) $(OBJDIR)/Gem5Wrapper.o

libramulator.a: $(OBJS) $(OBJDIR)/Gem5Wrapper.o $(OBJDIR)/ChampSimWrapper.o
	$(AR) $(ARFLAGS) $@ $^

# libramulator_champsim.a: $(OBJS) $(OBJDIR)/ChampSimWrapper.o
# 	$(AR) $(ARFLAGS) $@ $^

$(OBJS): | $(OBJDIR)

$(OBJDIR): 
	@mkdir -p $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -DRAMULATOR $(USER_DEFINES) -c -o $@ $<
