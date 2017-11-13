// Copyright 2016 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ctime>
#include <sstream>
#include <string>

#include "firebase/app.h"
#include "firebase/auth.h"
#include "firebase/util.h"

// Thin OS abstraction layer.
#include "main.h"  // NOLINT

using ::firebase::App;
using ::firebase::AppOptions;
using ::firebase::Future;
using ::firebase::FutureBase;
using ::firebase::auth::Auth;
using ::firebase::auth::Credential;
using ::firebase::auth::AuthError;
using ::firebase::auth::kAuthErrorNone;
using ::firebase::auth::kAuthErrorFailure;
using ::firebase::auth::EmailAuthProvider;
using ::firebase::auth::FacebookAuthProvider;
using ::firebase::auth::GitHubAuthProvider;
using ::firebase::auth::GoogleAuthProvider;
using ::firebase::auth::OAuthProvider;
using ::firebase::auth::PhoneAuthProvider;
using ::firebase::auth::TwitterAuthProvider;
using ::firebase::auth::User;
using ::firebase::auth::UserInfoInterface;

// Set this to true, and set the email/password, to test a custom email address.
static const bool kTestCustomEmail = false;
static const char kCustomEmail[] = "custom.email@example.com";
static const char kCustomPassword[] = "CustomPasswordGoesHere";

// Constants used during tests.
static const char kTestPassword[] = "testEmailPassword123";
static const char kTestEmailBad[] = "bad.test.email@example.com";
static const char kTestPasswordBad[] = "badTestPassword";
static const char kTestIdTokenBad[] = "bad id token for testing";
static const char kTestAccessTokenBad[] = "bad access token for testing";
static const char kTestPasswordUpdated[] = "testpasswordupdated";
static const char kTestIdProviderIdBad[] = "bad provider id for testing";

static const int kWaitIntervalMs = 300;
static const int kPhoneAuthCodeSendWaitMs = 600000;
static const int kPhoneAuthCompletionWaitMs = 8000;
static const int kPhoneAuthTimeoutMs = 0;

static const char kFirebaseProviderId[] =
#if defined(__ANDROID__)
    "firebase";
#else   // !defined(__ANDROID__)
    "Firebase";
#endif  // !defined(__ANDROID__)

// Don't return until `future` is complete.
// Print a message for whether the result mathes our expectations.
// Returns true if the application should exit.
static bool WaitForFuture(FutureBase future, const char* fn,
                          AuthError expected_error, bool log_error = true) {
  // Note if the future has not be started properly.
  if (future.status() == ::firebase::kFutureStatusInvalid) {
    LogMessage("ERROR: Future for %s is invalid", fn);
    return false;
  }

  // Wait for future to complete.
  LogMessage("  Calling %s...", fn);
  while (future.status() == ::firebase::kFutureStatusPending) {
    if (ProcessEvents(100)) return true;
  }

  // Log error result.
  if (log_error) {
    const AuthError error = static_cast<AuthError>(future.error());
    if (error == expected_error) {
      const char* error_message = future.error_message();
      if (error_message) {
        LogMessage("%s completed as expected", fn);
      } else {
        LogMessage("%s completed as expected, error: %d '%s'", fn, error,
                   error_message);
      }
    } else {
      LogMessage("ERROR: %s completed with error: %d, `%s`", fn, error,
                 future.error_message());
    }
  }
  return false;
}

static bool WaitForSignInFuture(Future<User*> sign_in_future, const char* fn,
                                AuthError expected_error, Auth* auth) {
  if (WaitForFuture(sign_in_future, fn, expected_error)) return true;

  const User* const* sign_in_user_ptr = sign_in_future.result();
  const User* sign_in_user =
      sign_in_user_ptr == nullptr ? nullptr : *sign_in_user_ptr;
  const User* auth_user = auth->current_user();

  if (sign_in_user != auth_user) {
    LogMessage("ERROR: future's user (%x) and current_user (%x) don't match",
               static_cast<int>(reinterpret_cast<intptr_t>(sign_in_user)),
               static_cast<int>(reinterpret_cast<intptr_t>(auth_user)));
  }

  return false;
}

// Wait for the current user to sign out.  Typically you should use the
// state listener to determine whether the user has signed out.
static bool WaitForSignOut(firebase::auth::Auth* auth) {
  while (auth->current_user() != nullptr) {
    if (ProcessEvents(100)) return true;
  }
  // Wait - hopefully - long enough for listeners to be signalled.
  ProcessEvents(1000);
  return false;
}

// Create an email that will be different from previous runs.
// Useful for testing creating new accounts.
static std::string CreateNewEmail() {
  std::stringstream email;
  email << "random_" << std::time(0) << "@gmail.com";
  return email.str();
}

static void ExpectFalse(const char* test, bool value) {
  if (value) {
    LogMessage("ERROR: %s is true instead of false", test);
  } else {
    LogMessage("%s is false, as expected", test);
  }
}

static void ExpectTrue(const char* test, bool value) {
  if (value) {
    LogMessage("%s is true, as expected", test);
  } else {
    LogMessage("ERROR: %s is false instead of true", test);
  }
}

// Log results of a string comparison for `test`.
static void ExpectStringsEqual(const char* test, const char* expected,
                               const char* actual) {
  if (strcmp(expected, actual) == 0) {
    LogMessage("%s is '%s' as expected", test, actual);
  } else {
    LogMessage("ERROR: %s is '%s' instead of '%s'", test, actual, expected);
  }
}

class AuthStateChangeCounter : public firebase::auth::AuthStateListener {
 public:
  AuthStateChangeCounter() : num_state_changes_(0) {}

  virtual void OnAuthStateChanged(Auth* auth) {  // NOLINT
    num_state_changes_++;
    LogMessage("OnAuthStateChanged User %p (state changes %d)",
               auth->current_user(), num_state_changes_);
  }

  void CompleteTest(const char* test_name, int expected_state_changes) {
    CompleteTest(test_name, expected_state_changes, expected_state_changes);
  }

  void CompleteTest(const char* test_name, int min_state_changes,
                    int max_state_changes) {
    const bool success = min_state_changes <= num_state_changes_ &&
                         num_state_changes_ <= max_state_changes;
    LogMessage("%sAuthStateListener called %d time%s on %s.",
               success ? "" : "ERROR: ", num_state_changes_,
               num_state_changes_ == 1 ? "" : "s", test_name);
    num_state_changes_ = 0;
  }

 private:
  int num_state_changes_;
};

class IdTokenChangeCounter : public firebase::auth::IdTokenListener {
 public:
  IdTokenChangeCounter() : num_token_changes_(0) {}

  virtual void OnIdTokenChanged(Auth* auth) {  // NOLINT
    num_token_changes_++;
    LogMessage("OnIdTokenChanged User %p (token changes %d)",
               auth->current_user(), num_token_changes_);
  }

  void CompleteTest(const char* test_name, int token_changes) {
    CompleteTest(test_name, token_changes, token_changes);
  }

  void CompleteTest(const char* test_name, int min_token_changes,
                    int max_token_changes) {
    const bool success = min_token_changes <= num_token_changes_ &&
                         num_token_changes_ <= max_token_changes;
    LogMessage("%sIdTokenListener called %d time%s on %s.",
               success ? "" : "ERROR: ", num_token_changes_,
               num_token_changes_ == 1 ? "" : "s", test_name);
    num_token_changes_ = 0;
  }

 private:
  int num_token_changes_;
};

// Utility class for holding a user's login credentials.
class UserLogin {
 public:
  UserLogin(Auth* auth, const std::string& email, const std::string& password)
      : auth_(auth),
        email_(email),
        password_(password),
        user_(nullptr),
        log_errors_(true) {}

  explicit UserLogin(Auth* auth) : auth_(auth) {
    email_ = CreateNewEmail();
    password_ = kTestPassword;
  }

  ~UserLogin() {
    if (user_ != nullptr) {
      log_errors_ = false;
      Delete();
    }
  }

  void Register() {
    Future<User*> register_test_account =
        auth_->CreateUserWithEmailAndPassword(email(), password());
    WaitForSignInFuture(register_test_account,
                        "CreateUserWithEmailAndPassword() to create temp user",
                        kAuthErrorNone, auth_);
    user_ = register_test_account.result() ? *register_test_account.result()
                                           : nullptr;
  }

  void Login() {
    Credential email_cred =
        EmailAuthProvider::GetCredential(email(), password());
    Future<User*> sign_in_cred = auth_->SignInWithCredential(email_cred);
    WaitForSignInFuture(sign_in_cred,
                        "Auth::SignInWithCredential() for UserLogin",
                        kAuthErrorNone, auth_);
  }

  void Delete() {
    if (user_ != nullptr) {
      Future<void> delete_future = user_->Delete();
      if (delete_future.status() == ::firebase::kFutureStatusInvalid) {
        Login();
        delete_future = user_->Delete();
      }

      WaitForFuture(delete_future, "User::Delete()", kAuthErrorNone,
                    log_errors_);
    }
    user_ = nullptr;
  }

  const char* email() const { return email_.c_str(); }
  const char* password() const { return password_.c_str(); }
  User* user() const { return user_; }
  void set_email(const char* email) { email_ = email; }
  void set_password(const char* password) { password_ = password; }

 private:
  Auth* auth_;
  std::string email_;
  std::string password_;
  User* user_;
  bool log_errors_;
};

class PhoneListener : public PhoneAuthProvider::Listener {
 public:
  PhoneListener()
      : num_calls_on_verification_complete_(0),
        num_calls_on_verification_failed_(0),
        num_calls_on_code_sent_(0),
        num_calls_on_code_auto_retrieval_time_out_(0) {}

  void OnVerificationCompleted(Credential /*credential*/) override {
    LogMessage("PhoneListener: successful automatic verification.");
    num_calls_on_verification_complete_++;
  }

  void OnVerificationFailed(const std::string& error) override {
    LogMessage("ERROR: PhoneListener verification failed with error, %s",
               error.c_str());
    num_calls_on_verification_failed_++;
  }

  void OnCodeSent(const std::string& verification_id,
                  const PhoneAuthProvider::ForceResendingToken&
                      force_resending_token) override {
    LogMessage("PhoneListener: code sent. verification_id=%s",
               verification_id.c_str());
    verification_id_ = verification_id;
    force_resending_token_ = force_resending_token;
    num_calls_on_code_sent_++;
  }

  void OnCodeAutoRetrievalTimeOut(const std::string& verification_id) override {
    LogMessage("PhoneListener: auto retrieval timeout. verification_id=%s",
               verification_id.c_str());
    verification_id_ = verification_id;
    num_calls_on_code_auto_retrieval_time_out_++;
  }

  const std::string& verification_id() const { return verification_id_; }
  const PhoneAuthProvider::ForceResendingToken& force_resending_token() const {
    return force_resending_token_;
  }
  int num_calls_on_verification_complete() const {
    return num_calls_on_verification_complete_;
  }
  int num_calls_on_verification_failed() const {
    return num_calls_on_verification_failed_;
  }
  int num_calls_on_code_sent() const { return num_calls_on_code_sent_; }
  int num_calls_on_code_auto_retrieval_time_out() const {
    return num_calls_on_code_auto_retrieval_time_out_;
  }

 private:
  std::string verification_id_;
  PhoneAuthProvider::ForceResendingToken force_resending_token_;
  int num_calls_on_verification_complete_;
  int num_calls_on_verification_failed_;
  int num_calls_on_code_sent_;
  int num_calls_on_code_auto_retrieval_time_out_;
};

// Execute all methods of the C++ Auth API.
extern "C" int common_main(int argc, const char* argv[]) {
  App* app;
  LogMessage("Starting Auth tests.");

#if defined(__ANDROID__)
  app = App::Create(GetJniEnv(), GetActivity());
#else
  app = App::Create();
#endif  // defined(__ANDROID__)

  LogMessage("Created the Firebase app %x.",
             static_cast<int>(reinterpret_cast<intptr_t>(app)));
  // Create the Auth class for that App.

  ::firebase::ModuleInitializer initializer;
  initializer.Initialize(app, nullptr, [](::firebase::App* app, void*) {
    ::firebase::InitResult init_result;
    Auth::GetAuth(app, &init_result);
    return init_result;
  });
  while (initializer.InitializeLastResult().status() !=
         firebase::kFutureStatusComplete) {
    if (ProcessEvents(100)) return 1;  // exit if requested
  }

  if (initializer.InitializeLastResult().error() != 0) {
    LogMessage("Failed to initialize Auth: %s",
               initializer.InitializeLastResult().error_message());
    ProcessEvents(2000);
    return 1;
  }

  Auth* auth = Auth::GetAuth(app);

  LogMessage("Created the Auth %x class for the Firebase app.",
             static_cast<int>(reinterpret_cast<intptr_t>(auth)));

  // It's possible for current_user() to be non-null if the previous run
  // left us in a signed-in state.
  if (auth->current_user() == nullptr) {
    LogMessage("No user signed in at creation time.");
  } else {
    LogMessage("Current user %s already signed in, so signing them out.",
               auth->current_user()->display_name().c_str());
    auth->SignOut();
  }

  // --- Credential copy tests -------------------------------------------------
  {
    Credential email_cred =
        EmailAuthProvider::GetCredential(kCustomEmail, kCustomPassword);
    Credential facebook_cred =
        FacebookAuthProvider::GetCredential(kTestAccessTokenBad);

    // Test copy constructor.
    Credential cred_copy(email_cred);

    // Test assignment operator.
    cred_copy = facebook_cred;
    (void)cred_copy;
  }

  // --- Custom Profile tests --------------------------------------------------
  {
    if (kTestCustomEmail) {
      // Test Auth::SignInWithEmailAndPassword().
      // Sign in with email and password that have already been registered.
      Future<User*> sign_in_future =
          auth->SignInWithEmailAndPassword(kCustomEmail, kCustomPassword);
      WaitForSignInFuture(sign_in_future,
                          "Auth::SignInWithEmailAndPassword() existing "
                          "(custom) email and password",
                          kAuthErrorNone, auth);
      // Test SignOut() after signed in with email and password.
      if (sign_in_future.status() == ::firebase::kFutureStatusComplete) {
        auth->SignOut();
        if (auth->current_user() != nullptr) {
          LogMessage(
              "ERROR: current_user() returning %x instead of nullptr after "
              "SignOut()",
              auth->current_user());
        }
      }
    }
  }

  // --- StateChange tests -----------------------------------------------------
  {
    AuthStateChangeCounter counter;
    IdTokenChangeCounter token_counter;

    // Test notification on registration.
    auth->AddAuthStateListener(&counter);
    auth->AddIdTokenListener(&token_counter);
    counter.CompleteTest("registration", 0);
    token_counter.CompleteTest("registration", 0);

    // Test notification on SignOut(), when already signed-out.
    auth->SignOut();
    counter.CompleteTest("SignOut() when already signed-out", 0);
    token_counter.CompleteTest("SignOut() when already signed-out", 0);

    // Test notification on SignIn().
    Future<User*> sign_in_future = auth->SignInAnonymously();
    WaitForSignInFuture(sign_in_future, "Auth::SignInAnonymously()",
                        kAuthErrorNone, auth);
    // Notified when the user is about to change and after the user has
    // changed.
    counter.CompleteTest("SignInAnonymously()", 2, 4);
    token_counter.CompleteTest("SignInAnonymously()", 2, 5);

    // Refresh the token.
    if (auth->current_user() != nullptr) {
      Future<std::string> token_future = auth->current_user()->GetToken(true);
      WaitForFuture(token_future, "GetToken()", kAuthErrorNone);
      counter.CompleteTest("GetToken()", 0);
      token_counter.CompleteTest("GetToken()", 1);
    }

    // Test notification on SignOut(), when signed-in.
    LogMessage("Current user %p", auth->current_user());  // DEBUG
    auth->SignOut();
    // Wait for the sign out to complete.
    WaitForSignOut(auth);
    counter.CompleteTest("SignOut()", 1);
    token_counter.CompleteTest("SignOut()", 1);
    LogMessage("Current user %p", auth->current_user());  // DEBUG

    auth->RemoveAuthStateListener(&counter);
    auth->RemoveIdTokenListener(&token_counter);
  }

  // --- PhoneListener tests ---------------------------------------------------
  {
    UserLogin user_login(auth);  // Generate a random name/password
    user_login.Register();

    LogMessage("Verifying phone number");

    const std::string phone_number = ReadTextInput(
        "Phone Number", "Please enter your phone number", "+12345678900");
    PhoneListener listener;
    PhoneAuthProvider& phone_provider = PhoneAuthProvider::GetInstance(auth);
    phone_provider.VerifyPhoneNumber(phone_number.c_str(), kPhoneAuthTimeoutMs,
                                     nullptr, &listener);

    // Wait for OnCodeSent() callback.
    int wait_ms = 0;
    while (listener.num_calls_on_verification_complete() == 0 &&
           listener.num_calls_on_verification_failed() == 0 &&
           listener.num_calls_on_code_sent() == 0) {
      if (wait_ms > kPhoneAuthCodeSendWaitMs) break;
      ProcessEvents(kWaitIntervalMs);
      wait_ms += kWaitIntervalMs;
      LogMessage(".");
    }
    if (wait_ms > kPhoneAuthCodeSendWaitMs) {
      LogMessage("ERROR: SMS with verification code not sent.");
    } else {
      LogMessage("SMS verification code sent.");

      const std::string verification_code = ReadTextInput(
          "Verification Code",
          "Please enter the verification code sent to you via SMS", "123456");

      // Wait for one of the other callbacks.
      while (listener.num_calls_on_verification_complete() == 0 &&
             listener.num_calls_on_verification_failed() == 0 &&
             listener.num_calls_on_code_auto_retrieval_time_out() == 0) {
        if (wait_ms > kPhoneAuthCompletionWaitMs) break;
        ProcessEvents(kWaitIntervalMs);
        wait_ms += kWaitIntervalMs;
        LogMessage(".");
      }
      if (listener.num_calls_on_code_auto_retrieval_time_out() > 0) {
        const Credential phone_credential = phone_provider.GetCredential(
            listener.verification_id().c_str(), verification_code.c_str());

        Future<User*> phone_future =
            auth->SignInWithCredential(phone_credential);
        WaitForSignInFuture(phone_future,
                            "Auth::SignInWithCredential() phone credential",
                            kAuthErrorNone, auth);
      } else {
        LogMessage("ERROR: SMS auto-detect time out did not occur.");
      }
    }
  }

  // --- Auth tests ------------------------------------------------------------
  {
    UserLogin user_login(auth);  // Generate a random name/password
    user_login.Register();
    if (!user_login.user()) {
      LogMessage("ERROR: Could not register new user.");
    } else {
      // Test Auth::SignInAnonymously().
      {
        Future<User*> sign_in_future = auth->SignInAnonymously();
        WaitForSignInFuture(sign_in_future, "Auth::SignInAnonymously()",
                            kAuthErrorNone, auth);
        ExpectTrue("SignInAnonymouslyLastResult matches returned Future",
                   sign_in_future == auth->SignInAnonymouslyLastResult());

        // Test SignOut() after signed in anonymously.
        if (sign_in_future.status() == ::firebase::kFutureStatusComplete) {
          auth->SignOut();
          if (auth->current_user() != nullptr) {
            LogMessage(
                "ERROR: current_user() returning %x instead of nullptr after "
                "SignOut()",
                auth->current_user());
          }
        }
      }

      // Test Auth::FetchProvidersForEmail().
      {
        Future<Auth::FetchProvidersResult> providers_future =
            auth->FetchProvidersForEmail(user_login.email());
        WaitForFuture(providers_future, "Auth::FetchProvidersForEmail()",
                      kAuthErrorNone);
        ExpectTrue(
            "FetchProvidersForEmailLastResult matches returned Future",
            providers_future == auth->FetchProvidersForEmailLastResult());

        const Auth::FetchProvidersResult* pro = providers_future.result();
        if (pro) {
          LogMessage("  email %s, num providers %d", user_login.email(),
                     pro->providers.size());
          for (auto it = pro->providers.begin(); it != pro->providers.end();
               ++it) {
            LogMessage("    * %s", it->c_str());
          }
        }
      }

      // Test Auth::SignInWithEmailAndPassword().
      // Sign in with email and password that have already been registered.
      {
        Future<User*> sign_in_future = auth->SignInWithEmailAndPassword(
            user_login.email(), user_login.password());
        WaitForSignInFuture(
            sign_in_future,
            "Auth::SignInWithEmailAndPassword() existing email and password",
            kAuthErrorNone, auth);
        ExpectTrue(
            "SignInWithEmailAndPasswordLastResult matches returned Future",
            sign_in_future == auth->SignInWithEmailAndPasswordLastResult());

        // Test SignOut() after signed in with email and password.
        if (sign_in_future.status() == ::firebase::kFutureStatusComplete) {
          auth->SignOut();
          if (auth->current_user() != nullptr) {
            LogMessage(
                "ERROR: current_user() returning %x instead of nullptr after "
                "SignOut()",
                auth->current_user());
          }
        }
      }

      // Sign in user with bad email. Should fail.
      {
        Future<User*> sign_in_future_bad_email =
            auth->SignInWithEmailAndPassword(kTestEmailBad, kTestPassword);
        WaitForSignInFuture(sign_in_future_bad_email,
                            "Auth::SignInWithEmailAndPassword() bad email",
                            ::firebase::auth::kAuthErrorUserNotFound, auth);
      }

      // Sign in user with correct email but bad password. Should fail.
      {
        Future<User*> sign_in_future_bad_password =
            auth->SignInWithEmailAndPassword(user_login.email(),
                                             kTestPasswordBad);
        WaitForSignInFuture(sign_in_future_bad_password,
                            "Auth::SignInWithEmailAndPassword() bad password",
                            ::firebase::auth::kAuthErrorWrongPassword, auth);
      }

      // Try to create with existing email. Should fail.
      {
        Future<User*> create_future_bad = auth->CreateUserWithEmailAndPassword(
            user_login.email(), user_login.password());
        WaitForSignInFuture(
            create_future_bad,
            "Auth::CreateUserWithEmailAndPassword() existing email",
            ::firebase::auth::kAuthErrorEmailAlreadyInUse, auth);
        ExpectTrue(
            "CreateUserWithEmailAndPasswordLastResult matches returned Future",
            create_future_bad ==
                auth->CreateUserWithEmailAndPasswordLastResult());
      }

      // Test Auth::SignInWithCredential() using email&password.
      // Use existing email. Should succeed.
      {
        Credential email_cred_ok = EmailAuthProvider::GetCredential(
            user_login.email(), user_login.password());
        Future<User*> sign_in_cred_ok =
            auth->SignInWithCredential(email_cred_ok);
        WaitForSignInFuture(sign_in_cred_ok,
                            "Auth::SignInWithCredential() existing email",
                            kAuthErrorNone, auth);
        ExpectTrue("SignInWithCredentialLastResult matches returned Future",
                   sign_in_cred_ok == auth->SignInWithCredentialLastResult());
      }

      // Use bad Facebook credentials. Should fail.
      {
        Credential facebook_cred_bad =
            FacebookAuthProvider::GetCredential(kTestAccessTokenBad);
        Future<User*> facebook_bad =
            auth->SignInWithCredential(facebook_cred_bad);
        WaitForSignInFuture(
            facebook_bad,
            "Auth::SignInWithCredential() bad Facebook credentials",
            ::firebase::auth::kAuthErrorOperationNotAllowed, auth);
      }

      // Use bad GitHub credentials. Should fail.
      {
        Credential git_hub_cred_bad =
            GitHubAuthProvider::GetCredential(kTestAccessTokenBad);
        Future<User*> git_hub_bad =
            auth->SignInWithCredential(git_hub_cred_bad);
        WaitForSignInFuture(
            git_hub_bad, "Auth::SignInWithCredential() bad GitHub credentials",
            ::firebase::auth::kAuthErrorOperationNotAllowed, auth);
      }

      // Use bad Google credentials. Should fail.
      {
        Credential google_cred_bad = GoogleAuthProvider::GetCredential(
            kTestIdTokenBad, kTestAccessTokenBad);
        Future<User*> google_bad = auth->SignInWithCredential(google_cred_bad);
        WaitForSignInFuture(
            google_bad, "Auth::SignInWithCredential() bad Google credentials",
            kAuthErrorFailure, auth);
      }

      // Use bad Google credentials, missing an optional parameter. Should fail.
      {
        Credential google_cred_bad =
            GoogleAuthProvider::GetCredential(kTestIdTokenBad, nullptr);
        Future<User*> google_bad = auth->SignInWithCredential(google_cred_bad);
        WaitForSignInFuture(
            google_bad, "Auth::SignInWithCredential() bad Google credentials",
            kAuthErrorFailure, auth);
      }

      // Use bad Twitter credentials. Should fail.
      {
        Credential twitter_cred_bad = TwitterAuthProvider::GetCredential(
            kTestIdTokenBad, kTestAccessTokenBad);
        Future<User*> twitter_bad =
            auth->SignInWithCredential(twitter_cred_bad);
        WaitForSignInFuture(
            twitter_bad, "Auth::SignInWithCredential() bad Twitter credentials",
            ::firebase::auth::kAuthErrorOperationNotAllowed, auth);
      }

      // Use bad OAuth credentials. Should fail.
      {
        Credential oauth_cred_bad = OAuthProvider::GetCredential(
            kTestIdProviderIdBad, kTestIdTokenBad, kTestAccessTokenBad);
        Future<User*> oauth_bad = auth->SignInWithCredential(oauth_cred_bad);
        WaitForSignInFuture(
            oauth_bad, "Auth::SignInWithCredential() bad OAuth credentials",
            kAuthErrorFailure, auth);
      }

      // Test Auth::SendPasswordResetEmail().
      // Use existing email. Should succeed.
      {
        Future<void> send_password_reset_ok =
            auth->SendPasswordResetEmail(user_login.email());
        WaitForFuture(send_password_reset_ok,
                      "Auth::SendPasswordResetEmail() existing email",
                      kAuthErrorNone);
        ExpectTrue(
            "SendPasswordResetEmailLastResult matches returned Future",
            send_password_reset_ok == auth->SendPasswordResetEmailLastResult());
      }

      // Use bad email. Should fail.
      {
        Future<void> send_password_reset_bad =
            auth->SendPasswordResetEmail(kTestEmailBad);
        WaitForFuture(send_password_reset_bad,
                      "Auth::SendPasswordResetEmail() bad email",
                      ::firebase::auth::kAuthErrorUserNotFound);
      }
    }
  }
  // --- User tests ------------------------------------------------------------
  // Test anonymous user info strings.
  {
    Future<User*> anon_sign_in_for_user = auth->SignInAnonymously();
    WaitForSignInFuture(anon_sign_in_for_user,
                        "Auth::SignInAnonymously() for User", kAuthErrorNone,
                        auth);
    if (anon_sign_in_for_user.status() == ::firebase::kFutureStatusComplete) {
      User* anonymous_user = anon_sign_in_for_user.result()
                                 ? *anon_sign_in_for_user.result()
                                 : nullptr;
      if (anonymous_user != nullptr) {
        LogMessage("Anonymous uid is %s", anonymous_user->uid().c_str());
        ExpectStringsEqual("Anonymous user email", "",
                           anonymous_user->email().c_str());
        ExpectStringsEqual("Anonymous user display_name", "",
                           anonymous_user->display_name().c_str());
        ExpectStringsEqual("Anonymous user photo_url", "",
                           anonymous_user->photo_url().c_str());
        ExpectStringsEqual("Anonymous user provider_id", kFirebaseProviderId,
                           anonymous_user->provider_id().c_str());
        ExpectTrue("Anonymous user is_anonymous()",
                   anonymous_user->is_anonymous());
        ExpectFalse("Anonymous user is_email_verified()",
                    anonymous_user->is_email_verified());
        ExpectTrue("Anonymous user metadata().last_sign_in_timestamp != 0",
                   anonymous_user->metadata().last_sign_in_timestamp != 0);
        ExpectTrue("Anonymous user metadata().creation_timestamp != 0",
                   anonymous_user->metadata().creation_timestamp != 0);

        // Test User::LinkWithCredential(), linking with email & password.
        const std::string newer_email = CreateNewEmail();
        Credential user_cred = EmailAuthProvider::GetCredential(
            newer_email.c_str(), kTestPassword);
        {
          Future<User*> link_future =
              anonymous_user->LinkWithCredential(user_cred);
          WaitForSignInFuture(link_future, "User::LinkWithCredential()",
                              kAuthErrorNone, auth);
        }

        // Test User::LinkWithCredential(), linking with same email & password.
        {
          Future<User*> link_future =
              anonymous_user->LinkWithCredential(user_cred);
          WaitForSignInFuture(link_future, "User::LinkWithCredential() again",
                              kAuthErrorNone, auth);
        }

        // Test User::LinkWithCredential(), linking with bad credential.
        // Call should fail and Auth's current user should be maintained.
        {
          const User* pre_link_user = auth->current_user();
          ExpectTrue("Test precondition requires active user",
                     pre_link_user != nullptr);

          Credential twitter_cred_bad = TwitterAuthProvider::GetCredential(
              kTestIdTokenBad, kTestAccessTokenBad);
          Future<User*> link_bad_future =
              anonymous_user->LinkWithCredential(twitter_cred_bad);
          WaitForFuture(link_bad_future,
                        "User::LinkWithCredential() with bad credential",
                        ::firebase::auth::kAuthErrorOperationNotAllowed);
          ExpectTrue("Linking maintains user",
                     auth->current_user() == pre_link_user);
        }

        // Test Auth::SignInWithCredential(), signing in with bad credential.
        // Call should fail, and Auth's current user should be maintained.
        {
          const User* pre_signin_user = auth->current_user();
          ExpectTrue("Test precondition requires active user",
                     pre_signin_user != nullptr);
          Credential twitter_cred_bad = TwitterAuthProvider::GetCredential(
              kTestIdTokenBad, kTestAccessTokenBad);
          Future<User*> signin_bad_future =
              auth->SignInWithCredential(twitter_cred_bad);
          WaitForFuture(signin_bad_future,
                        "Auth::SignInWithCredential() with bad credential",
                        ::firebase::auth::kAuthErrorOperationNotAllowed, auth);
          ExpectTrue("Failed sign in maintains user",
                     auth->current_user() == pre_signin_user);
        }

        UserLogin user_login(auth);
        user_login.Register();

        if (!user_login.user()) {
          LogMessage("Error - Could not create new user.");
        } else {
          // Test email user info strings.
          Future<User*> email_sign_in_for_user =
              auth->SignInWithEmailAndPassword(user_login.email(),
                                               user_login.password());
          WaitForSignInFuture(email_sign_in_for_user,
                              "Auth::SignInWithEmailAndPassword() for User",
                              kAuthErrorNone, auth);
          User* email_user = email_sign_in_for_user.result()
                                 ? *email_sign_in_for_user.result()
                                 : nullptr;
          if (email_user != nullptr) {
            LogMessage("Email uid is %s", email_user->uid().c_str());
            ExpectStringsEqual("Email user email", user_login.email(),
                               email_user->email().c_str());
            ExpectStringsEqual("Email user display_name", "",
                               email_user->display_name().c_str());
            ExpectStringsEqual("Email user photo_url", "",
                               email_user->photo_url().c_str());
            ExpectStringsEqual("Email user provider_id", kFirebaseProviderId,
                               email_user->provider_id().c_str());
            ExpectFalse("Email user is_anonymous()",
                        email_user->is_anonymous());
            ExpectFalse("Email user is_email_verified()",
                        email_user->is_email_verified());
            ExpectTrue("Email user metadata().last_sign_in_timestamp != 0",
                       email_user->metadata().last_sign_in_timestamp != 0);
            ExpectTrue("Email user metadata().creation_timestamp  != 0",
                       email_user->metadata().creation_timestamp != 0);

            // Test User::GetToken().
            // with force_refresh = false.
            Future<std::string> token_no_refresh = email_user->GetToken(false);
            WaitForFuture(token_no_refresh, "User::GetToken(false)",
                          kAuthErrorNone);
            LogMessage("User::GetToken(false) = %s",
                       token_no_refresh.result()
                           ? token_no_refresh.result()->c_str()
                           : "");

            // with force_refresh = true.
            Future<std::string> token_force_refresh =
                email_user->GetToken(true);
            WaitForFuture(token_force_refresh, "User::GetToken(true)",
                          kAuthErrorNone);
            LogMessage("User::GetToken(true) = %s",
                       token_force_refresh.result()
                           ? token_force_refresh.result()->c_str()
                           : "");

            // Test Reload().
            Future<void> reload_future = email_user->Reload();
            WaitForFuture(reload_future, "User::Reload()", kAuthErrorNone);

            // Test User::Unlink().
            Future<User*> unlink_future = email_user->Unlink("firebase");
            WaitForSignInFuture(unlink_future, "User::Unlink()",
                                ::firebase::auth::kAuthErrorNoSuchProvider,
                                auth);

            // Sign in again if user is now invalid.
            if (auth->current_user() == nullptr) {
              Future<User*> email_sign_in_again =
                  auth->SignInWithEmailAndPassword(user_login.email(),
                                                   user_login.password());
              WaitForSignInFuture(email_sign_in_again,
                                  "Auth::SignInWithEmailAndPassword() again",
                                  kAuthErrorNone, auth);
              email_user = email_sign_in_again.result()
                               ? *email_sign_in_again.result()
                               : nullptr;
            }
          }
          if (email_user != nullptr) {
            // Test User::provider_data().
            const std::vector<UserInfoInterface*>& provider_data =
                email_user->provider_data();
            LogMessage("User::provider_data() returned %d interface%s",
                       provider_data.size(),
                       provider_data.size() == 1 ? "" : "s");
            for (size_t i = 0; i < provider_data.size(); ++i) {
              const UserInfoInterface* user_info = provider_data[i];
              LogMessage(
                  "    UID() = %s\n"
                  "    Email() = %s\n"
                  "    DisplayName() = %s\n"
                  "    PhotoUrl() = %s\n"
                  "    ProviderId() = %s",
                  user_info->uid().c_str(), user_info->email().c_str(),
                  user_info->display_name().c_str(),
                  user_info->photo_url().c_str(),
                  user_info->provider_id().c_str());
            }

            // Test User::UpdateEmail().
            const std::string newest_email = CreateNewEmail();
            Future<void> update_email_future =
                email_user->UpdateEmail(newest_email.c_str());
            WaitForFuture(update_email_future, "User::UpdateEmail()",
                          kAuthErrorNone);

            // Test User::UpdatePassword().
            Future<void> update_password_future =
                email_user->UpdatePassword(kTestPasswordUpdated);
            WaitForFuture(update_password_future, "User::UpdatePassword()",
                          kAuthErrorNone);

            // Test User::Reauthenticate().
            Credential email_cred_reauth = EmailAuthProvider::GetCredential(
                newest_email.c_str(), kTestPasswordUpdated);
            Future<void> reauthenticate_future =
                email_user->Reauthenticate(email_cred_reauth);
            WaitForFuture(reauthenticate_future, "User::Reauthenticate()",
                          kAuthErrorNone);

            // Test User::SendEmailVerification().
            Future<void> send_email_verification_future =
                email_user->SendEmailVerification();
            WaitForFuture(send_email_verification_future,
                          "User::SendEmailVerification()", kAuthErrorNone);
          }
        }
      }
    }

    // Test User::Delete().
    const std::string new_email_for_delete = CreateNewEmail();
    Future<User*> create_future_for_delete =
        auth->CreateUserWithEmailAndPassword(new_email_for_delete.c_str(),
                                             kTestPassword);
    WaitForSignInFuture(
        create_future_for_delete,
        "Auth::CreateUserWithEmailAndPassword() new email for delete",
        kAuthErrorNone, auth);
    User* email_user_for_delete = create_future_for_delete.result()
                                      ? *create_future_for_delete.result()
                                      : nullptr;
    if (email_user_for_delete != nullptr) {
      Future<void> delete_future = email_user_for_delete->Delete();
      WaitForFuture(delete_future, "User::Delete()", kAuthErrorNone);
    }
  }
  {
    // We end with a login so that we can test if a second run will detect
    // that we're already logged-in.
    Future<User*> sign_in_future = auth->SignInAnonymously();
    WaitForSignInFuture(sign_in_future, "Auth::SignInAnonymously() at end",
                        kAuthErrorNone, auth);
  }

  LogMessage("Completed Auth tests.");

  while (!ProcessEvents(1000)) {
  }

  delete auth;
  delete app;

  return 0;
}
