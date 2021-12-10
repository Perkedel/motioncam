package com.motioncam.ui;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.lifecycle.ViewModelProvider;

import com.motioncam.CameraActivity;
import com.motioncam.R;
import com.motioncam.databinding.SettingsFragmentBinding;
import com.motioncam.model.SettingsViewModel;

import java.util.Locale;

public class SettingsFragment extends Fragment {
    private SettingsViewModel mViewModel;
    private ActivityResultLauncher<Intent> mSelectDocumentLauncher;
    private SettingsFragmentBinding mDataBinding;

    public static SettingsFragment newInstance() {
        return new SettingsFragment();
    }

    private void setupObservers(SettingsFragmentBinding dataBinding) {
        mViewModel.memoryUseMb.observe(getViewLifecycleOwner(), (value) -> {
            value = SettingsViewModel.MINIMUM_MEMORY_USE_MB + value;
            dataBinding.memoryUseText.setText(String.format(Locale.US, "%d MB", value));

            mViewModel.save(requireContext());
        });

        mViewModel.rawVideoMemoryUseMb.observe(getViewLifecycleOwner(), (value) -> {
            value = SettingsViewModel.MINIMUM_MEMORY_USE_MB + value;
            dataBinding.rawVideoMemoryUseText.setText(String.format(Locale.US, "%d MB", value));

            mViewModel.save(requireContext());
        });

        mViewModel.jpegQuality.observe(getViewLifecycleOwner(), (value) -> {
            dataBinding.jpegQualityText.setText(String.format(Locale.US, "%d%%", value));

            mViewModel.save(requireContext());
        });

        mViewModel.cameraPreviewQuality.observe(getViewLifecycleOwner(), (value) -> {
            switch(value) {
                case 0:
                    dataBinding.cameraQualityPreviewText.setText(getString(R.string.low));
                    break;

                case 1:
                    dataBinding.cameraQualityPreviewText.setText(getString(R.string.medium));
                    break;

                case 2:
                    dataBinding.cameraQualityPreviewText.setText(getString(R.string.high));
                    break;
            }

            mViewModel.save(requireContext());
        });

        mViewModel.dualExposureControls.observe(getViewLifecycleOwner(), (value) -> {
            dataBinding.cameraQualitySeekBar.setEnabled(value);

            mViewModel.save(requireContext());
        });

        mViewModel.autoNightMode.observe(getViewLifecycleOwner(), (value) -> mViewModel.save(requireContext()));
        mViewModel.raw10.observe(getViewLifecycleOwner(), (value) -> mViewModel.save(requireContext()));
        mViewModel.raw16.observe(getViewLifecycleOwner(), (value) -> mViewModel.save(requireContext()));
        mViewModel.rawVideoToDng.observe(getViewLifecycleOwner(), (value) -> mViewModel.save(requireContext()));

        mViewModel.rawVideoTempStorageFolder.observe(getViewLifecycleOwner(), (value) -> {
            mDataBinding.rawVideoStorageFolder.setText(value);
            mViewModel.save(requireContext());
        });
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater,
                             @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {

        return inflater.inflate(R.layout.settings_fragment, container, false);
    }

    public void onAttach(@NonNull Context context) {
        super.onAttach(context);

        // Set up folder selection
        mSelectDocumentLauncher = registerForActivityResult(
                new ActivityResultContracts.StartActivityForResult(),
                result -> {
                    if (result.getResultCode() == Activity.RESULT_OK) {
                        Intent data = result.getData();
                        if (data == null || data.getData() == null) {
                            return;
                        }

                        final int takeFlags =
                                Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;

                        Uri uri = data.getData();

                        // Save location
                        ContentResolver contentResolver = requireContext().getContentResolver();

                        // Release previous permission if set
                        String prevUri = mViewModel.rawVideoTempStorageFolder.getValue();
                        if(prevUri != null) {
                            Log.i(CameraActivity.TAG, "Releasing permissions for " + prevUri);
                            contentResolver.releasePersistableUriPermission(Uri.parse(prevUri), takeFlags);
                        }

                        contentResolver.takePersistableUriPermission(uri, takeFlags);

                        mViewModel.rawVideoTempStorageFolder.setValue(uri.toString());
                    }
                });
    }

    @Override
    public void onActivityCreated(@Nullable Bundle savedInstanceState) {
        super.onActivityCreated(savedInstanceState);

        mViewModel = new ViewModelProvider(this).get(SettingsViewModel.class);

        // Bind to model
        mDataBinding = SettingsFragmentBinding.bind(requireView().findViewById(R.id.settingsLayout));

        mDataBinding.setViewModel(mViewModel);
        mDataBinding.setLifecycleOwner(this);

        // Update maximum memory use
        ActivityManager activityManager = (ActivityManager) requireContext().getSystemService(Context.ACTIVITY_SERVICE);
        ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();

        activityManager.getMemoryInfo(memInfo);

        long totalMemory = memInfo.totalMem / (1024 * 1024) - (SettingsViewModel.MINIMUM_MEMORY_USE_MB*2);
        int maxMemory = Math.min( (int) totalMemory, SettingsViewModel.MAXIMUM_MEMORY_USE_MB);

        mDataBinding.memoryUseSeekBar.setMax(maxMemory - SettingsViewModel.MINIMUM_MEMORY_USE_MB);
        mDataBinding.rawVideoMemoryUseSeekBar.setMax(maxMemory - SettingsViewModel.MINIMUM_MEMORY_USE_MB);

        mDataBinding.rawVideoStorageSelectBtn.setOnClickListener((v) -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            mSelectDocumentLauncher.launch(intent);
        });

        SharedPreferences sharedPrefs =
                requireContext().getSharedPreferences(SettingsViewModel.CAMERA_SHARED_PREFS, Context.MODE_PRIVATE);

        String rawVideoStorageLocation =
                sharedPrefs.getString(SettingsViewModel.PREFS_KEY_RAW_VIDEO_TEMP_OUTPUT_URI, "");

        mDataBinding.rawVideoStorageFolder.setText(rawVideoStorageLocation);

        // Set up observers
        setupObservers(mDataBinding);

        mViewModel.load(requireContext());
    }

    @Override
    public void onPause() {
        super.onPause();
    }
}
