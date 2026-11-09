#include <components/sdlutil/startupsignals.hpp>

#include <gtest/gtest.h>
#include <stdexcept>

namespace
{
    using Handler = void (*)(int);
    void applicationHandler(int) {}

    struct SignalState
    {
        int mSignal;
        Handler mPrevious;
        SignalState(int signal, Handler handler)
            : mSignal(signal)
            , mPrevious(std::signal(signal, handler))
        {
        }
        ~SignalState() { std::signal(mSignal, mPrevious); }
    };

    Handler currentHandler(int signal)
    {
        const auto result = std::signal(signal, SIG_DFL);
        std::signal(signal, result);
        return result;
    }

    TEST(StartupSignals, TerminationDoesNotDependOnEventPumpingDuringBootstrap)
    {
        const SignalState interrupt(SIGINT, applicationHandler), terminate(SIGTERM, applicationHandler);
        {
            SDLUtil::ScopedStartupSignals startup;
            EXPECT_EQ(currentHandler(SIGINT), SIG_DFL);
            EXPECT_EQ(currentHandler(SIGTERM), SIG_DFL);
        }
        EXPECT_EQ(currentHandler(SIGINT), applicationHandler);
        EXPECT_EQ(currentHandler(SIGTERM), applicationHandler);
    }

    TEST(StartupSignals, InheritedIgnoredSignalsStayIgnored)
    {
        const SignalState interrupt(SIGINT, SIG_IGN), terminate(SIGTERM, applicationHandler);
        {
            SDLUtil::ScopedStartupSignals startup;
            EXPECT_EQ(currentHandler(SIGINT), SIG_IGN);
            EXPECT_EQ(currentHandler(SIGTERM), SIG_DFL);
        }
        EXPECT_EQ(currentHandler(SIGINT), SIG_IGN);
        EXPECT_EQ(currentHandler(SIGTERM), applicationHandler);
    }

    TEST(StartupSignals, FailedInitializationRestoresPreviousHandlers)
    {
        const SignalState interrupt(SIGINT, applicationHandler), terminate(SIGTERM, applicationHandler);
        EXPECT_THROW(
            {
                SDLUtil::ScopedStartupSignals startup;
                throw std::runtime_error("initialization failed");
            },
            std::runtime_error);
        EXPECT_EQ(currentHandler(SIGINT), applicationHandler);
        EXPECT_EQ(currentHandler(SIGTERM), applicationHandler);
    }

    TEST(StartupSignals, CrashReporterFatalSignalHandlersAreNotChanged)
    {
        const SignalState fatal(SIGABRT, applicationHandler);
        {
            SDLUtil::ScopedStartupSignals startup;
            EXPECT_EQ(currentHandler(SIGABRT), applicationHandler);
        }
        EXPECT_EQ(currentHandler(SIGABRT), applicationHandler);
    }

#ifndef _WIN32
    TEST(StartupSignals, PosixActionMaskAndFlagsAreRestored)
    {
        struct RestoreAction
        {
            struct sigaction mPrevious{};
            RestoreAction() { ::sigaction(SIGTERM, nullptr, &mPrevious); }
            ~RestoreAction() { ::sigaction(SIGTERM, &mPrevious, nullptr); }
        } restore;
        struct sigaction action{};
        action.sa_handler = applicationHandler;
        ::sigemptyset(&action.sa_mask);
        ::sigaddset(&action.sa_mask, SIGUSR1);
        action.sa_flags = SA_RESTART;
        ASSERT_EQ(::sigaction(SIGTERM, &action, nullptr), 0);
        {
            SDLUtil::ScopedStartupSignals startup;
        }
        struct sigaction after{};
        ASSERT_EQ(::sigaction(SIGTERM, nullptr, &after), 0);
        EXPECT_EQ(after.sa_handler, applicationHandler);
        EXPECT_NE(after.sa_flags & SA_RESTART, 0);
        EXPECT_EQ(::sigismember(&after.sa_mask, SIGUSR1), 1);
    }
#endif
}
