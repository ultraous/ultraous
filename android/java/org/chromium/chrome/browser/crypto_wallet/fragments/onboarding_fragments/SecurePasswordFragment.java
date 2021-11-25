/* Copyright (c) 2021 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.chromium.chrome.browser.crypto_wallet.fragments.onboarding_fragments;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.hardware.biometrics.BiometricPrompt;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.text.TextUtils;
import android.view.LayoutInflater;
import android.view.MotionEvent;
import android.view.View;
import android.view.View.OnFocusChangeListener;
import android.view.ViewGroup;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.RequiresApi;
import androidx.core.content.ContextCompat;

import org.chromium.brave_wallet.mojom.KeyringController;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.crypto_wallet.activities.BraveWalletActivity;
import org.chromium.chrome.browser.crypto_wallet.util.KeystoreHelper;
import org.chromium.chrome.browser.crypto_wallet.util.Utils;

import java.util.concurrent.Executor;

public class SecurePasswordFragment extends CryptoOnboardingFragment {

    private KeyringController getKeyringController() {
        Activity activity = getActivity();
        if (activity instanceof BraveWalletActivity) {
            return ((BraveWalletActivity) activity).getKeyringController();
        }

        return null;
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }

    @Override
    public View onCreateView(
            LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        View view = inflater.inflate(R.layout.fragment_secure_password, container, false);
        view.setOnTouchListener(new View.OnTouchListener() {
            @Override
            @SuppressLint("ClickableViewAccessibility")
            public boolean onTouch(View v, MotionEvent event) {
                if (event.getAction() == MotionEvent.ACTION_DOWN) {
                    InputMethodManager inputMethodManager =
                            (InputMethodManager) getActivity().getSystemService(
                                    Activity.INPUT_METHOD_SERVICE);
                    inputMethodManager.hideSoftInputFromWindow(
                            getActivity().getCurrentFocus().getWindowToken(), 0);
                }
                return true;
            }
        });
        return view;
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        Button secureCryptoButton = view.findViewById(R.id.btn_secure_crypto_continue);
        secureCryptoButton.setOnClickListener(v -> {
            EditText passwordEdittext = view.findViewById(R.id.secure_crypto_password);
            String passwordInput = passwordEdittext.getText().toString();

            KeyringController keyringController = getKeyringController();
            assert keyringController != null;
            keyringController.isStrongPassword(passwordInput, result -> {
                if (!result) {
                    passwordEdittext.setError(getResources().getString(R.string.password_text));

                    return;
                }
                proceedWithAStrongPassword(passwordInput, view);
            });
        });
    }

    private void proceedWithAStrongPassword(String passwordInput, View view) {
        EditText retypePasswordEdittext = view.findViewById(R.id.secure_crypto_retype_password);
        String retypePasswordInput = retypePasswordEdittext.getText().toString();
        if (!passwordInput.equals(retypePasswordInput)) {
            retypePasswordEdittext.setError(
                    getResources().getString(R.string.retype_password_error));
        } else {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                final BiometricPrompt.AuthenticationCallback authenticationCallback =
                        new BiometricPrompt.AuthenticationCallback() {
                            private void onNextPage() {
                                // Go to next Page
                                goToTheNextPage(passwordInput);
                            }

                            @Override
                            public void onAuthenticationSucceeded(
                                    BiometricPrompt.AuthenticationResult result) {
                                super.onAuthenticationSucceeded(result);
                                KeystoreHelper.useBiometricOnUnlock(
                                        passwordInput);
                                onNextPage();
                            }

                            @Override
                            public void onAuthenticationError(
                                    int errorCode, CharSequence errString) {
                                super.onAuthenticationError(errorCode, errString);

                                // Even though we have an error, we still let to proceed
                                Toast.makeText(getActivity(), errString, Toast.LENGTH_SHORT).show();
                                onNextPage();
                            }
                        };
                showFingerprintDialog(authenticationCallback);
            } else {
                goToTheNextPage(passwordInput);
            }
        }
    }

    private void goToTheNextPage(String passwordInput) {
        KeyringController keyringController = getKeyringController();
        if (keyringController != null) {
            keyringController.createWallet(passwordInput,
                    recoveryPhrases
                    -> {
                            // Do nothing with recovery phrase for now
                    });
        }
        Utils.setCryptoOnboarding(false);
        onNextPage.gotoNextPage(false);
    }

    @RequiresApi(api = Build.VERSION_CODES.P)
    private void showFingerprintDialog(
            @NonNull final BiometricPrompt.AuthenticationCallback authenticationCallback) {
        assert getActivity() != null;
        Executor executor = ContextCompat.getMainExecutor(getActivity());
        new BiometricPrompt.Builder(getActivity())
                .setTitle(getResources().getString(R.string.enable_fingerprint_unlock))
                .setDescription(getResources().getString(R.string.enable_fingerprint_text))
                .setNegativeButton(getResources().getString(android.R.string.cancel), executor,
                        (dialog, which)
                                -> authenticationCallback.onAuthenticationError(
                                        BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED, ""))
                .build()
                .authenticate(new CancellationSignal(), executor, authenticationCallback);
    }
}
