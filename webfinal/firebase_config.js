// firebase_config.js (Firebase v10 compat)

const firebaseConfig = {
  apiKey: "AIzaSyBROuJFSiU3D3i2Viu8MqkK4mfAqCzYhy0",
  authDomain: "bh1750-c60fb.firebaseapp.com",
  databaseURL: "https://bh1750-c60fb-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "bh1750-c60fb",
  storageBucket: "bh1750-c60fb.firebasestorage.app",
  messagingSenderId: "267215158593",
  appId: "1:267215158593:web:7c7f01f8fa82f78799b5c0",
  measurementId: "G-9K42VR4TCX"
};

if (!window.firebase) throw new Error("Firebase SDK not loaded");

if (!firebase.apps.length) firebase.initializeApp(firebaseConfig);

window.fbApp  = firebase.app();
window.fbDB   = firebase.database();
window.fbAuth = (firebase.auth ? firebase.auth() : null);
