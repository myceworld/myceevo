#ifndef POS_WALLET_H
#define POS_WALLET_H

#include <wallet/wallet.h>

namespace wallet {
   struct WalletContext;
   class CWallet;
}

class CStakeWallet;
extern CStakeWallet stakeWallet;

/**
 * Convenience class allowing stake functions to have easy access to the wallet,
 * without the linking issues that come with later bitcoin releases.
 */
class CStakeWallet
{
    private:
        bool ready;
        wallet::CWallet* wallet;

    public:
        CStakeWallet()
        {
            ready = false;
            wallet = nullptr;
        }

        bool IsReady() { return ready; }
        void SetReady() { ready = true; }
        void UnsetReady() { ready = false; }

        void AttachWallet(wallet::CWallet* pwallet) {
            if (!pwallet) return;
            wallet = pwallet;
            SetReady();
        }

        void RemoveWallet() {
            wallet = nullptr;
            UnsetReady();
        }

        wallet::CWallet* GetStakingWallet();
};

#endif // POS_WALLET_H
