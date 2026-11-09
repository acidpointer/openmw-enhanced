#ifndef OPENMW_COMPONENTS_SDLUTIL_STARTUPSIGNALS_H
#define OPENMW_COMPONENTS_SDLUTIL_STARTUPSIGNALS_H

#include <csignal>
#ifndef _WIN32
#include <signal.h>
#endif

namespace SDLUtil
{
    // SDL translates termination signals into events. Before the event loop
    // runs, a blocking driver call would prevent those events being consumed.
    // Keep POSIX termination available during bootstrap and restore the exact
    // previous handlers when normal event processing begins. Fatal-signal
    // handlers used by the crash reporter are deliberately not changed.
    class ScopedStartupSignals
    {
    public:
        ScopedStartupSignals()
            : mInterrupt(SIGINT)
            , mTerminate(SIGTERM)
        {
        }
        ScopedStartupSignals(const ScopedStartupSignals&) = delete;
        ScopedStartupSignals& operator=(const ScopedStartupSignals&) = delete;

    private:
        class SavedSignal
        {
        public:
            explicit SavedSignal(int signal)
                : mSignal(signal)
            {
#ifdef _WIN32
                mPrevious = std::signal(signal, SIG_DFL);
                if (mPrevious == SIG_IGN)
                    std::signal(signal, SIG_IGN);
#else
                // Query first so an inherited SIG_IGN is never transiently
                // replaced. Preserve the complete action, including its mask.
                if (::sigaction(signal, nullptr, &mPrevious) != 0 || mPrevious.sa_handler == SIG_IGN)
                    return;
                struct sigaction action{};
                action.sa_handler = SIG_DFL;
                ::sigemptyset(&action.sa_mask);
                mChanged = ::sigaction(signal, &action, nullptr) == 0;
#endif
            }
            ~SavedSignal()
            {
#ifdef _WIN32
                if (mPrevious != SIG_ERR)
                    std::signal(mSignal, mPrevious);
#else
                if (mChanged)
                    ::sigaction(mSignal, &mPrevious, nullptr);
#endif
            }

        private:
            int mSignal;
#ifdef _WIN32
            using Handler = void (*)(int);
            Handler mPrevious = SIG_ERR;
#else
            struct sigaction mPrevious{};
            bool mChanged = false;
#endif
        };
        SavedSignal mInterrupt;
        SavedSignal mTerminate;
    };
}

#endif
