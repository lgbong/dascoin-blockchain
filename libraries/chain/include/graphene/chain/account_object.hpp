/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once
#include <graphene/chain/protocol/operations.hpp>
#include <graphene/chain/license_objects.hpp>
#include <graphene/chain/upgrade_type.hpp>
#include <graphene/db/generic_index.hpp>
#include <boost/multi_index/composite_key.hpp>

namespace graphene { namespace chain {
   class database;

   /**
    * @class account_statistics_object
    * @ingroup object
    * @ingroup implementation
    *
    * This object contains regularly updated statistical data about an account. It is provided for the purpose of
    * separating the account data that changes frequently from the account data that is mostly static, which will
    * minimize the amount of data that must be backed up as part of the undo history everytime a transfer is made.
    */
   class account_statistics_object : public graphene::db::abstract_object<account_statistics_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_account_statistics_object_type;

         account_id_type  owner;

         /**
          * Keep the most recent operation as a root pointer to a linked list of the transaction history.
          */
         account_transaction_history_id_type most_recent_op;
         uint32_t                            total_ops = 0;

         /**
          * When calculating votes it is necessary to know how much is stored in orders (and thus unavailable for
          * transfers). Rather than maintaining an index of [asset,owner,order_id] we will simply maintain the running
          * total here and update it every time an order is created or modified.
          */
         share_type total_core_in_orders;

         /**
          * Tracks the total fees paid by this account for the purpose of calculating bulk discounts.
          */
         share_type lifetime_fees_paid;

         /**
          * Tracks the fees paid by this account which have not been disseminated to the various parties that receive
          * them yet (registrar, referrer, lifetime referrer, network, etc). This is used as an optimization to avoid
          * doing massive amounts of uint128 arithmetic on each and every operation.
          *
          * These fees will be paid out as vesting cash-back, and this counter will reset during the maintenance
          * interval.
          */
         share_type pending_fees;
         /**
          * Same as @ref pending_fees, except these fees will be paid out as pre-vested cash-back (immediately
          * available for withdrawal) rather than requiring the normal vesting period.
          */
         share_type pending_vested_fees;

         /// @brief Split up and pay out @ref pending_fees and @ref pending_vested_fees
         void process_fees(const account_object& a, database& d) const;

         /**
          * Core fees are paid into the account_statistics_object by this method
          */
         void pay_fee( share_type core_fee, share_type cashback_vesting_threshold );

         /**
          * Do all operations on a regular db maintenance cycle.
          * @param db A reference to the object database.
          * @param license_type A reference to the license type object.
          * @param dgpo A reference to the dynamic global properties object.
          */
         void process_db_maintenance(database& db,
                                     const optional<license_type_id_type> license_type,
                                     const dynamic_global_property_object& dgpo);
   };

   /**
    * @brief Tracks the balance of a single account/asset pair
    * @ingroup object
    *
    * This object is indexed on owner and asset_type so that black swan
    * events in asset_type can be processed quickly.
    */
   class account_balance_object : public abstract_object<account_balance_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_account_balance_object_type;

         account_id_type   owner;
         asset_id_type     asset_type;
         share_type        balance;
         share_type reserved = 0;
         share_type spent = 0;  // Balance spent in limit interval.

         share_type eur_limit;  // The limit in euros for this balance.
         share_type limit;  // The limit used for transfers on this balance.

         asset get_balance() const { return asset{balance, asset_type}; }
         asset get_reserved_balance() const { return asset{reserved, asset_type}; }
         asset_reserved get_asset_reserved_balance() const { return asset_reserved{balance, reserved, asset_type}; }

         asset get_spent_balance() const { return asset{spent, asset_type}; }
         asset get_limit() const { return asset{limit, asset_type}; }

         void  adjust_balance(const asset& delta);
   };

   /**
    * @brief Tracks the cycle balance of a single account and connects it to a license.
    * @ingroup object
    *
    */
   class account_cycle_balance_object : public abstract_object<account_cycle_balance_object>
   {
      public:
         static const uint8_t space_id = implementation_ids;
         static const uint8_t type_id  = impl_account_cycle_balance_object_type;

         account_id_type owner;
         share_type balance;

         share_type get_balance() const { return balance; }
   };

   /**
    * @brief This class represents an account on the object graph
    * @ingroup object
    * @ingroup protocol
    *
    * Accounts are the primary unit of authority on the graphene system. Users must have an account in order to use
    * assets, trade in the markets, vote for committee_members, etc.
    */
   class account_object : public graphene::db::abstract_object<account_object>
   {
      public:
         static const uint8_t space_id = protocol_ids;
         static const uint8_t type_id  = account_object_type;

         /**
          * What kind of account is this: wallet, vault, system or other?
          */
         account_kind kind;

         /**
          * How deep in the hierarchy is this account? For now the depth is:
          * 0 - wallet or vault without tethering
          * 1 - vault tethered to a wallet
          * Greater values should not be allowed.
          */
         uint8_t hierarchy_depth = 0;

         /**
          * This set contains all vault accounts tethered to this wallet account. For all other account kinds it must be
          * empty.
          */
         flat_set<account_id_type> vault;

         /**
          * This set contains all wallet parents for a certain vault account. For all other account kinds it must be
          * empty.
          */
         flat_set<account_id_type> parents;

         /**
          * The time at which this account's membership expires.
          * If set to any time in the past, the account is a basic account.
          * If set to time_point_sec::maximum(), the account is a lifetime member.
          * If set to any time not in the past less than time_point_sec::maximum(), the account is an annual member.
          *
          * See @ref is_lifetime_member, @ref is_basic_account, @ref is_annual_member, and @ref is_member
          */
         time_point_sec membership_expiration_date;

         ///The account that paid the fee to register this account. Receives a percentage of referral rewards.
         account_id_type registrar;
         /// The account credited as referring this account. Receives a percentage of referral rewards.
         account_id_type referrer;
         /// The lifetime member at the top of the referral tree. Receives a percentage of referral rewards.
         account_id_type lifetime_referrer;

         /// Percentage of fee which should go to network.
         uint16_t network_fee_percentage = GRAPHENE_DEFAULT_NETWORK_PERCENT_OF_FEE;
         /// Percentage of fee which should go to lifetime referrer.
         uint16_t lifetime_referrer_fee_percentage = 0;
         /// Percentage of referral rewards (leftover fee after paying network and lifetime referrer) which should go
         /// to referrer. The remainder of referral rewards goes to the registrar.
         uint16_t referrer_rewards_percentage = 0;

         /// The account's name. This name must be unique among all account names on the graph. May not be empty.
         string name;

         /**
          * The owner authority represents absolute control over the account. Usually the keys in this authority will
          * be kept in cold storage, as they should not be needed very often and compromise of these keys constitutes
          * complete and irrevocable loss of the account. Generally the only time the owner authority is required is to
          * update the active authority.
          */
         authority owner;

         /// The owner_roll_back authority contains backup of owner key. This backup is used roll_back_public_key operation.
         authority owner_roll_back;

         /// This one will track the number of times the owner has been changed.
         uint32_t owner_change_counter = 0;

         /// The owner authority contains the hot keys of the account. This authority has control over nearly all
         /// operations the account may perform.
         authority active;

         /// The active_roll_back authority contains backup of active key. This backup is used roll_back_public_key operation.
         authority active_roll_back;

         /// This one will track the number of times the active authority has been changed.
         uint32_t active_change_counter = 0;

         /// This flag is tracking whether account has opted in or out from roll_back_public_key feature.
         bool roll_back_enabled = true;

         /// This flag is set to true when account roll_back_public_key operation is done. No other operation
         /// but change_public_keys is possible, which when completed, resets this flag to false.
         bool roll_back_active = false;

         typedef account_options  options_type;
         account_options options;
         /// The reference implementation records the account's statistics in a separate object. This field contains the
         /// ID of that object.
         account_statistics_id_type statistics;

         /**
          * This is a set of all accounts which have 'whitelisted' this account. Whitelisting is only used in core
          * validation for the purpose of authorizing accounts to hold and transact in whitelisted assets. This
          * account cannot update this set, except by transferring ownership of the account, which will clear it. Other
          * accounts may add or remove their IDs from this set.
          */
         flat_set<account_id_type> whitelisting_accounts;

         /**
          * Optionally track all of the accounts this account has whitelisted or blacklisted, these should
          * be made Immutable so that when the account object is cloned no deep copy is required.  This state is
          * tracked for GUI display purposes.
          *
          * TODO: move white list tracking to its own multi-index container rather than having 4 fields on an
          * account.   This will scale better because under the current design if you whitelist 2000 accounts,
          * then every time someone fetches this account object they will get the full list of 2000 accounts.
          */
         ///@{
         set<account_id_type> whitelisted_accounts;
         set<account_id_type> blacklisted_accounts;
         ///@}


         /**
          * This is a set of all accounts which have 'blacklisted' this account. Blacklisting is only used in core
          * validation for the purpose of forbidding accounts from holding and transacting in whitelisted assets. This
          * account cannot update this set, and it will be preserved even if the account is transferred. Other accounts
          * may add or remove their IDs from this set.
          */
         flat_set<account_id_type> blacklisting_accounts;

         /**
          * Vesting balance which receives cashback_reward deposits.
          */
         optional<vesting_balance_id_type> cashback_vb;

         special_authority owner_special_authority = no_special_authority();
         special_authority active_special_authority = no_special_authority();

         /**
          * History of license purshases, max license, frequency lock and upgrades.
          */
         optional<license_information_id_type> license_information;

         /**
          * The level of verified personal information assigned to the account.
          */
         uint8_t pi_level;

         /**
          * If this is true, vault account observes no limits on transferring vault to wallet.
          **/
         bool disable_vault_to_wallet_limit = false;

         /**
          * This flag is set when the top_n logic sets both authorities,
          * and gets reset when authority or special_authority is set.
          */
         uint8_t top_n_control_flags = 0;
         static const uint8_t top_n_control_owner  = 1;
         static const uint8_t top_n_control_active = 2;

         /**
          * This is a set of assets which the account is allowed to have.
          * This is utilized to restrict buyback accounts to the assets that trade in their markets.
          * In the future we may expand this to allow accounts to e.g. voluntarily restrict incoming transfers.
          */
         optional< flat_set<asset_id_type> > allowed_assets;

         bool has_special_authority()const
         {
            return (owner_special_authority.which() != special_authority::tag< no_special_authority >::value)
                || (active_special_authority.which() != special_authority::tag< no_special_authority >::value);
         }

         template<typename DB>
         const vesting_balance_object& cashback_balance(const DB& db)const
         {
            FC_ASSERT(cashback_vb);
            return db.get(*cashback_vb);
         }

         /// @return true if this is a lifetime member account; false otherwise.
         bool is_lifetime_member()const
         {
            return membership_expiration_date == time_point_sec::maximum();
         }
         /// @return true if this is a basic account; false otherwise.
         bool is_basic_account(time_point_sec now)const
         {
            return now > membership_expiration_date;
         }
         /// @return true if the account is an unexpired annual member; false otherwise.
         /// @note This method will return false for lifetime members.
         bool is_annual_member(time_point_sec now)const
         {
            return !is_lifetime_member() && !is_basic_account(now);
         }
         /// @return true if the account is an annual or lifetime member; false otherwise.
         bool is_member(time_point_sec now)const
         {
            return !is_basic_account(now);
         }
         /// @return true if the account is a wallet account.
         bool is_wallet()const
         {
            return kind == account_kind::wallet;
         }
         /// @return true if the account is a vault account.
         bool is_vault()const
         {
            return kind == account_kind::vault;
         }
         /// @return true if the account is a special account.
         bool is_special()const
         {
            return kind == account_kind::special;
         }
         /// @return true if account is a custodian wallet account.
         bool is_custodian() const
         {
        	 return kind == account_kind::custodian;
         }
         /// @return true if the account is in this accounts parent list
         bool has_in_parents(account_id_type account)const
         {
            return parents.find(account) != parents.end();
         }
         /// @return true if the account is in this accounts vault
         bool has_in_vault(account_id_type account)const
         {
            return vault.find(account) != vault.end();
         }

         account_id_type get_id()const { return id; }

         // TODO: use hierarchy depth here?
         bool is_tethered() const
         {
             return !vault.empty() || !parents.empty();
         }

         bool is_tethered_to(account_id_type acc_id) const
         {
             return vault.find(acc_id) != vault.end() || parents.find(acc_id) != parents.end();
         }
   };

   /**
    *  @brief This secondary index will allow a reverse lookup of all accounts that a particular key or account
    *  is an potential signing authority.
    */
   class account_member_index : public secondary_index
   {
      public:
         virtual void object_inserted( const object& obj ) override;
         virtual void object_removed( const object& obj ) override;
         virtual void about_to_modify( const object& before ) override;
         virtual void object_modified( const object& after  ) override;


         /** given an account or key, map it to the set of accounts that reference it in an active or owner authority */
         map< account_id_type, set<account_id_type> > account_to_account_memberships;
         map< public_key_type, set<account_id_type> > account_to_key_memberships;
         /** some accounts use address authorities in the genesis block */
         map< address, set<account_id_type> >         account_to_address_memberships;


      protected:
         set<account_id_type>  get_account_members( const account_object& a )const;
         set<public_key_type>  get_key_members( const account_object& a )const;
         set<address>          get_address_members( const account_object& a )const;

         set<account_id_type>  before_account_members;
         set<public_key_type>  before_key_members;
         set<address>          before_address_members;
   };


   /**
    *  @brief This secondary index will allow a reverse lookup of all accounts that have been referred by
    *  a particular account.
    */
   class account_referrer_index : public secondary_index
   {
      public:
         virtual void object_inserted( const object& obj ) override;
         virtual void object_removed( const object& obj ) override;
         virtual void about_to_modify( const object& before ) override;
         virtual void object_modified( const object& after  ) override;

         /** maps the referrer to the set of accounts that they have referred */
         map< account_id_type, set<account_id_type> > referred_by;
   };

   struct by_account_asset;
   struct by_asset_balance;
   /**
    * @ingroup object_index
    */
   typedef multi_index_container<
      account_balance_object,
      indexed_by<
         ordered_unique< tag<by_id>, member< object, object_id_type, &object::id > >,
         ordered_unique< tag<by_account_asset>,
            composite_key<
               account_balance_object,
               member<account_balance_object, account_id_type, &account_balance_object::owner>,
               member<account_balance_object, asset_id_type, &account_balance_object::asset_type>
            >
         >,
         ordered_unique< tag<by_asset_balance>,
            composite_key<
               account_balance_object,
               member<account_balance_object, asset_id_type, &account_balance_object::asset_type>,
               member<account_balance_object, share_type, &account_balance_object::balance>,
               member<account_balance_object, account_id_type, &account_balance_object::owner>
            >,
            composite_key_compare<
               std::less< asset_id_type >,
               std::greater< share_type >,
               std::less< account_id_type >
            >
         >
      >
   > account_balance_object_multi_index_type;

   /**
    * @ingroup object_index
    */
   typedef generic_index<account_balance_object, account_balance_object_multi_index_type> account_balance_index;

   struct by_name;
   typedef multi_index_container<
      account_object,
      indexed_by<
         ordered_unique< tag<by_id>,
            member< object, object_id_type, &object::id >
         >,
         ordered_unique< tag<by_name>,
            member<account_object, string, &account_object::name>
         >
      >
   > account_multi_index_type;

   /**
    * @ingroup object_index
    */
   typedef generic_index<account_object, account_multi_index_type> account_index;

   struct by_account_id;
   typedef multi_index_container<
      account_cycle_balance_object,
      indexed_by<
         ordered_unique< tag<by_id>,
            member< object, object_id_type, &object::id >
         >,
         ordered_non_unique< tag<by_account_id>,
            member< account_cycle_balance_object, account_id_type, &account_cycle_balance_object::owner>
         >
      >
   > account_cycle_balance_multi_index_type;

   typedef generic_index<
      account_cycle_balance_object, account_cycle_balance_multi_index_type
   > account_cycle_balance_index;

} }  // namsepace graphene::chain

FC_REFLECT_DERIVED( graphene::chain::account_object, (graphene::db::object),
                    (kind)
                    (hierarchy_depth)
                    (parents)
                    (vault)
                    (disable_vault_to_wallet_limit)
                    (membership_expiration_date)(registrar)(referrer)(lifetime_referrer)
                    (network_fee_percentage)(lifetime_referrer_fee_percentage)(referrer_rewards_percentage)
                    (name)(owner)(owner_change_counter)(active_change_counter)(active)(options)(statistics)(whitelisting_accounts)(blacklisting_accounts)
                    (owner_roll_back)(active_roll_back)(roll_back_enabled)(roll_back_active)
                    (whitelisted_accounts)(blacklisted_accounts)
                    (cashback_vb)
                    (owner_special_authority)
                    (active_special_authority)
                    (license_information)
                    (pi_level)
                    (top_n_control_flags)
                    (allowed_assets)
                  )

FC_REFLECT_DERIVED( graphene::chain::account_statistics_object, (graphene::chain::object),
                    (owner)
                    (most_recent_op)
                    (total_ops)
                    (total_core_in_orders)
                    (lifetime_fees_paid)
                    (pending_fees)
                    (pending_vested_fees)
                  )

FC_REFLECT_DERIVED( graphene::chain::account_balance_object, (graphene::db::object),
                    (owner)
                    (asset_type)
                    (balance)
                    (reserved)
                    (spent)
                    (eur_limit)
                    (limit)
                  )

FC_REFLECT_DERIVED( graphene::chain::account_cycle_balance_object, (graphene::db::object),
                    (owner)
                    (balance)
                  )
